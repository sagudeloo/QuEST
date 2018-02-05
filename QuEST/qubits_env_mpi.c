/** @file
An implementation of the API in qubits.h for an MPI environment.
*/
// defining prototype for gethostname
#define _BSD_SOURCE
#include <unistd.h>

# include <mpi.h>
# include <stdlib.h>
# include <stdio.h>
# include <math.h>
# include <omp.h>
# include "precision.h"
# include "qubits.h"
# include "qubits_internal.h"


# define DEBUG 0
static int isChunkToSkipInFindPZero(int chunkId, long long int chunkSize, int measureQubit);
static int chunkIsUpper(int chunkId, long long int chunkSize, int rotQubit);
static void getRotAngle(int chunkIsUpper, Complex *rot1, Complex *rot2, Complex alpha, Complex beta);
static int getChunkPairId(int chunkIsUpper, int chunkId, long long int chunkSize, int rotQubit);
static int halfMatrixBlockFitsInChunk(long long int chunkSize, int rotQubit);
static int getChunkIdFromIndex(MultiQubit multiQubit, long long int index);

void initQuESTEnv(QuESTEnv *env){
        // init MPI environment
        int rank, numRanks, initialized;
	MPI_Initialized(&initialized);
	if (!initialized){
		MPI_Init(NULL, NULL);
		MPI_Comm_size(MPI_COMM_WORLD, &numRanks);
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);

		if (DEBUG) {
			char hostName[256];
			int hostNameLen;
			MPI_Get_processor_name(hostName, &hostNameLen);
			printf("rank %d on host %s\n", rank, hostName);
		}
		env->rank=rank;
		env->numRanks=numRanks;
	} else printf("ERROR: Trying to initialize QuESTEnv multiple times. Ignoring\n");
}

void syncQuESTEnv(QuESTEnv env){
	MPI_Barrier(MPI_COMM_WORLD);
}

int syncQuESTSuccess(int successCode){
	int totalSuccess;
	MPI_Allreduce(&successCode, &totalSuccess, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);
	return totalSuccess;
}

void closeQuESTEnv(QuESTEnv env){
	int finalized;
	MPI_Finalized(&finalized);
	if (!finalized) MPI_Finalize();
	else printf("ERROR: Trying to close QuESTEnv multiple times. Ignoring\n");
}

void reportQuESTEnv(QuESTEnv env){
	if (env.rank==0){
		printf("EXECUTION ENVIRONMENT:\n"); 
		printf("Running distributed (MPI) version\n");
		printf("Number of ranks is %d\n", env.numRanks);
# ifdef _OPENMP
		printf("OpenMP enabled\n");
		printf("Number of threads available is %d\n", omp_get_max_threads());
# else
		printf("OpenMP disabled\n");
# endif 
		printf("Precision: size of REAL is %ld bytes\n", sizeof(REAL));
	}
}

void reportNodeList(QuESTEnv env){
	char hostName[256];
        gethostname(hostName, 255);
        printf("hostname on rank %d: %s\n", env.rank, hostName);
}

int getChunkIdFromIndex(MultiQubit multiQubit, long long int index){
	return index/multiQubit.numAmps; // this is numAmpsPerChunk
}

REAL getRealAmpEl(MultiQubit multiQubit, long long int index){
	int chunkId = getChunkIdFromIndex(multiQubit, index);
	REAL el; 
	if (multiQubit.chunkId==chunkId){
		el = multiQubit.stateVec.real[index-chunkId*multiQubit.numAmps];
	}
	MPI_Bcast(&el, 1, MPI_QuEST_REAL, chunkId, MPI_COMM_WORLD);
        return el; 
} 

REAL getImagAmpEl(MultiQubit multiQubit, long long int index){
	int chunkId = getChunkIdFromIndex(multiQubit, index);
	REAL el; 
	if (multiQubit.chunkId==chunkId){
		el = multiQubit.stateVec.imag[index-chunkId*multiQubit.numAmps];
	}
	MPI_Bcast(&el, 1, MPI_QuEST_REAL, chunkId, MPI_COMM_WORLD);
        return el; 
}

REAL calcTotalProbability(MultiQubit multiQubit){
  /* IJB - implemented using Kahan summation for greater accuracy at a slight floating
     point operation overhead. For more details see https://en.wikipedia.org/wiki/Kahan_summation_algorithm */
  /* Don't change the bracketing in this routine! */
  REAL pTotal=0; 
  REAL y, t, c;
  REAL allRankTotals=0;
  long long int index;
  long long int numAmpsPerRank = multiQubit.numAmps;
  c = 0.0;
  for (index=0; index<numAmpsPerRank; index++){ 
    /* Perform pTotal+=multiQubit.stateVec.real[index]*multiQubit.stateVec.real[index]; by Kahan */
    y = multiQubit.stateVec.real[index]*multiQubit.stateVec.real[index] - c;
    t = pTotal + y;
    c = ( t - pTotal ) - y;
    pTotal = t;
    /* Perform pTotal+=multiQubit.stateVec.imag[index]*multiQubit.stateVec.imag[index]; by Kahan */
    y = multiQubit.stateVec.imag[index]*multiQubit.stateVec.imag[index] - c;
    t = pTotal + y;
    c = ( t - pTotal ) - y;
    pTotal = t;
  } 
  if (DEBUG) printf("before calc prob. %d\n", multiQubit.numChunks);
  if (multiQubit.numChunks>1) MPI_Allreduce(&pTotal, &allRankTotals, 1, MPI_QuEST_REAL, MPI_SUM, MPI_COMM_WORLD);
  else allRankTotals=pTotal;
  
  return allRankTotals;
}

/** Returns whether a given chunk in position chunkId is in the upper or lower half of
a block.

@param[in] chunkId id of chunk in state vector
@param[in] chunkSize number of amps in chunk
@param[in] rotQubit qubit being rotated 
@return 1: chunk is in upper half of block, 0: chunk is in lower half of block 
*/
//! fix -- is this the same as isChunkToSkip?
static int chunkIsUpper(int chunkId, long long int chunkSize, int rotQubit)
{       
        long long int sizeHalfBlock = 1LL << (rotQubit);
        long long int sizeBlock = sizeHalfBlock*2;
        long long int posInBlock = (chunkId*chunkSize) % sizeBlock;
        return posInBlock<sizeHalfBlock;
}

/** Get rotation values for a given chunk
@param[in] chunkIsUpper 1: chunk is in upper half of block, 0: chunk is in lower half

@param[out] rot1, rot2 rotation values to use, allocated for upper/lower such that
@verbatim
stateUpper = rot1 * stateUpper + conj(rot2)  * stateLower
@endverbatim
or
@verbatim
stateLower = rot1 * stateUpper + conj(rot2)  * stateLower
@endverbatim
@param[in] alpha, beta initial rotation values 
*/
static void getRotAngle(int chunkIsUpper, Complex *rot1, Complex *rot2, Complex alpha, Complex beta)
{
        if (chunkIsUpper){
                *rot1=alpha;
                rot2->real=-beta.real;
                rot2->imag=-beta.imag;
        } else {
                *rot1=beta;
                *rot2=alpha;
        }
}

/** Get rotation values for a given chunk given a unitary matrix
@param[in] chunkIsUpper 1: chunk is in upper half of block, 0: chunk is in lower half

@param[out] rot1, rot2 rotation values to use, allocated for upper/lower such that
@verbatim
stateUpper = rot1 * stateUpper + conj(rot2)  * stateLower
@endverbatim
or
@verbatim
stateLower = rot1 * stateUpper + conj(rot2)  * stateLower
@endverbatim
@param[in] u unitary matrix operation
*/
static void getRotAngleFromUnitaryMatrix(int chunkIsUpper, Complex *rot1, Complex *rot2, ComplexMatrix2 u)
{
        if (chunkIsUpper){
		*rot1=u.r0c0;
		*rot2=u.r0c1;
        } else {
		*rot1=u.r1c0;
		*rot2=u.r1c1;
        }
}

/** get position of corresponding chunk, holding values required to
update values in my chunk (with chunkId) when rotating rotQubit.

@param[in] chunkIsUpper 1: chunk is in upper half of block, 0: chunk is in lower half
@param[in] chunkId id of chunk in state vector
@param[in] chunkSize number of amps in chunk
@param[in] rotQubit qubit being rotated 
@return chunkId of chunk required to rotate rotQubit 
*/

static int getChunkPairId(int chunkIsUpper, int chunkId, long long int chunkSize, int rotQubit)
{
        long long int sizeHalfBlock = 1LL << (rotQubit);
        int chunksPerHalfBlock = sizeHalfBlock/chunkSize;
        if (chunkIsUpper){
                return chunkId + chunksPerHalfBlock;
        } else {
                return chunkId - chunksPerHalfBlock;
        }
}

/** return whether the current qubit rotation will use
blocks that fit within a single chunk.

@param[in] chunkSize number of amps in chunk
@param[in] rotQubit qubit being rotated 
@return 1: one chunk fits in one block 0: chunk is larger than block
*/

static int halfMatrixBlockFitsInChunk(long long int chunkSize, int rotQubit)
{
        long long int sizeHalfBlock = 1LL << (rotQubit);
        if (chunkSize > sizeHalfBlock) return 1;
        else return 0;
}

void exchangeStateVectors(MultiQubit multiQubit, int pairRank){
        // MPI send/receive vars
        int TAG=100;
        MPI_Status status;

	// Multiple messages are required as MPI uses int rather than long long int for count
	// For openmpi, messages are further restricted to 2GB in size -- do this for all cases
	// to be safe
	long long int maxMessageCount = 1LL<<29;
	if (sizeof(REAL)==8) maxMessageCount = (1LL<<28);
	else if (sizeof(REAL)==16) maxMessageCount = (1LL<<27);

	if (multiQubit.numAmps<maxMessageCount) maxMessageCount = multiQubit.numAmps;
	int numMessages = multiQubit.numAmps/maxMessageCount;
	int i;
	long long int offset;
	if (DEBUG) printf("numMessages %d maxMessageCount %lld\n", numMessages, maxMessageCount);

	// send my state vector to pairRank's multiQubit.pairStateVec
	// receive pairRank's state vector into multiQubit.pairStateVec
	for (i=0; i<numMessages; i++){
		offset = i*maxMessageCount;
		MPI_Sendrecv(&multiQubit.stateVec.real[offset], maxMessageCount, MPI_QuEST_REAL, pairRank, TAG,
				 &multiQubit.pairStateVec.real[offset], maxMessageCount, MPI_QuEST_REAL,
				 pairRank, TAG, MPI_COMM_WORLD, &status);
		//printf("rank: %d err: %d\n", multiQubit.rank, err);
		MPI_Sendrecv(&multiQubit.stateVec.imag[offset], maxMessageCount, MPI_QuEST_REAL, pairRank, TAG,
				&multiQubit.pairStateVec.imag[offset], maxMessageCount, MPI_QuEST_REAL,
				pairRank, TAG, MPI_COMM_WORLD, &status);
	}
}

void compactUnitary(MultiQubit multiQubit, const int rotQubit, Complex alpha, Complex beta)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);
        QuESTAssert(validateAlphaBeta(alpha, beta), 6, __func__);

        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);
        Complex rot1, rot2;

        // rank's chunk is in upper half of block 
        int rankIsUpper;
        int pairRank; // rank of corresponding chunk

        if (useLocalDataOnly){
                // all values required to update state vector lie in this rank
                compactUnitaryLocal(multiQubit, rotQubit, alpha, beta);
        } else {
                // need to get corresponding chunk of state vector from other rank
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                getRotAngle(rankIsUpper, &rot1, &rot2, alpha, beta);
                pairRank = getChunkPairId(rankIsUpper, multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                // get corresponding values from my pair
		        exchangeStateVectors(multiQubit, pairRank);

                // this rank's values are either in the upper of lower half of the block. 
		        // send values to compactUnitaryDistributed in the correct order
                if (rankIsUpper){
                        compactUnitaryDistributed(multiQubit,rotQubit,rot1,rot2,
                                multiQubit.stateVec, //upper
                                multiQubit.pairStateVec, //lower
                                multiQubit.stateVec); //output
                } else {
                        compactUnitaryDistributed(multiQubit,rotQubit,rot1,rot2,
                                multiQubit.pairStateVec, //upper
                                multiQubit.stateVec, //lower
                                multiQubit.stateVec); //output
                }
        }
}

void unitary(MultiQubit multiQubit, const int rotQubit, ComplexMatrix2 u)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);
        QuESTAssert(validateMatrixIsUnitary(u), 5, __func__);

        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);
        Complex rot1, rot2;

        // rank's chunk is in upper half of block 
        int rankIsUpper;
        int pairRank; // rank of corresponding chunk

        if (useLocalDataOnly){
                // all values required to update state vector lie in this rank
	        unitaryLocal(multiQubit, rotQubit, u);
        } else {
                // need to get corresponding chunk of state vector from other rank
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                getRotAngleFromUnitaryMatrix(rankIsUpper, &rot1, &rot2, u);
                pairRank = getChunkPairId(rankIsUpper, multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                // get corresponding values from my pair
		        exchangeStateVectors(multiQubit, pairRank);

                // this rank's values are either in the upper of lower half of the block. 
                // send values to compactUnitaryDistributed in the correct order
                if (rankIsUpper){
                        unitaryDistributed(multiQubit,rotQubit,rot1,rot2,
                                multiQubit.stateVec, //upper
                                multiQubit.pairStateVec, //lower
                                multiQubit.stateVec); //output
                } else {
                        unitaryDistributed(multiQubit,rotQubit,rot1,rot2,
                                multiQubit.pairStateVec, //upper
                                multiQubit.stateVec, //lower
                                multiQubit.stateVec); //output
                }
        }


}

void controlledCompactUnitary(MultiQubit multiQubit, const int rotQubit, const int controlQubit, Complex alpha, Complex beta)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);
        QuESTAssert(controlQubit >= 0 && controlQubit < multiQubit.numQubits, 2, __func__);
        QuESTAssert(controlQubit != rotQubit, 3, __func__);
        QuESTAssert(validateAlphaBeta(alpha, beta), 6, __func__);

        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);
        Complex rot1, rot2;

        // rank's chunk is in upper half of block 
        int rankIsUpper;
        int pairRank; // rank of corresponding chunk

        if (useLocalDataOnly){
                // all values required to update state vector lie in this rank
                controlledCompactUnitaryLocal(multiQubit, rotQubit, controlQubit, alpha, beta);
        } else {
                // need to get corresponding chunk of state vector from other rank
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                getRotAngle(rankIsUpper, &rot1, &rot2, alpha, beta);
                pairRank = getChunkPairId(rankIsUpper, multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                //printf("%d rank has pair rank: %d\n", multiQubit.rank, pairRank);
                // get corresponding values from my pair
		exchangeStateVectors(multiQubit, pairRank);
                
		// this rank's values are either in the upper of lower half of the block. send values to controlledCompactUnitaryDistributed
                // in the correct order
                if (rankIsUpper){
                        controlledCompactUnitaryDistributed(multiQubit,rotQubit,controlQubit,rot1,rot2,
                                multiQubit.stateVec, //upper
                                multiQubit.pairStateVec, //lower
                                multiQubit.stateVec); //output
                } else {
                        controlledCompactUnitaryDistributed(multiQubit,rotQubit,controlQubit,rot1,rot2,
                                multiQubit.pairStateVec, //upper
                                multiQubit.stateVec, //lower
                                multiQubit.stateVec); //output
                }
        }
}

void controlledUnitary(MultiQubit multiQubit, const int rotQubit, const int controlQubit, 
        ComplexMatrix2 u)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);
        QuESTAssert(controlQubit >= 0 && controlQubit < multiQubit.numQubits, 2, __func__);
        QuESTAssert(controlQubit != rotQubit, 3, __func__);
        QuESTAssert(validateMatrixIsUnitary(u), 5, __func__);

        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);
        Complex rot1, rot2;

        // rank's chunk is in upper half of block 
        int rankIsUpper;
        int pairRank; // rank of corresponding chunk

        if (useLocalDataOnly){
                // all values required to update state vector lie in this rank
                controlledUnitaryLocal(multiQubit, rotQubit, controlQubit, u);
        } else {
                // need to get corresponding chunk of state vector from other rank
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                getRotAngleFromUnitaryMatrix(rankIsUpper, &rot1, &rot2, u);
                pairRank = getChunkPairId(rankIsUpper, multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                //printf("%d rank has pair rank: %d\n", multiQubit.rank, pairRank);
                // get corresponding values from my pair
		exchangeStateVectors(multiQubit, pairRank);
                
		// this rank's values are either in the upper of lower half of the block. send values to controlledUnitaryDistributed
                // in the correct order
                if (rankIsUpper){
                        controlledUnitaryDistributed(multiQubit,rotQubit,controlQubit,rot1,rot2,
                                multiQubit.stateVec, //upper
                                multiQubit.pairStateVec, //lower
                                multiQubit.stateVec); //output
                } else {
                        controlledUnitaryDistributed(multiQubit,rotQubit,controlQubit,rot1,rot2,
                                multiQubit.pairStateVec, //upper
                                multiQubit.stateVec, //lower
                                multiQubit.stateVec); //output
                }
        }
}

void multiControlledUnitary(MultiQubit multiQubit, int* controlQubits, const int numControlQubits, const int rotQubit, ComplexMatrix2 u)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);
        QuESTAssert(numControlQubits >= 0 && numControlQubits < multiQubit.numQubits, 4, __func__);
        QuESTAssert(validateMatrixIsUnitary(u), 5, __func__);
         
        long long int mask=0;
        for (int i=0; i<numControlQubits; i++) mask = mask | (1LL<<controlQubits[i]);
        QuESTAssert(mask >=0 && mask <= (1LL<<multiQubit.numQubits)-1, 2, __func__);
        QuESTAssert((mask & (1LL<<rotQubit)) != (1LL<<rotQubit), 3, __func__);
        
        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);
        Complex rot1, rot2;

        // rank's chunk is in upper half of block 
        int rankIsUpper;
        int pairRank; // rank of corresponding chunk

        if (useLocalDataOnly){
                // all values required to update state vector lie in this rank
                multiControlledUnitaryLocal(multiQubit, rotQubit, mask, u);
        } else {
                // need to get corresponding chunk of state vector from other rank
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                getRotAngleFromUnitaryMatrix(rankIsUpper, &rot1, &rot2, u);
                pairRank = getChunkPairId(rankIsUpper, multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                //printf("%d rank has pair rank: %d\n", multiQubit.rank, pairRank);
                // get corresponding values from my pair
		exchangeStateVectors(multiQubit, pairRank);
                
		// this rank's values are either in the upper of lower half of the block. send values to multiControlledUnitaryDistributed
                // in the correct order
                if (rankIsUpper){
                        multiControlledUnitaryDistributed(multiQubit,rotQubit,mask,rot1,rot2,
                                multiQubit.stateVec, //upper
                                multiQubit.pairStateVec, //lower
                                multiQubit.stateVec); //output
                } else {
                        multiControlledUnitaryDistributed(multiQubit,rotQubit,mask,rot1,rot2,
                                multiQubit.pairStateVec, //upper
                                multiQubit.stateVec, //lower
                                multiQubit.stateVec); //output
                }
        }
}
void sigmaX(MultiQubit multiQubit, const int rotQubit)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);

        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);

        // rank's chunk is in upper half of block 
        int rankIsUpper;
        int pairRank; // rank of corresponding chunk

        if (useLocalDataOnly){
                // all values required to update state vector lie in this rank
                sigmaXLocal(multiQubit, rotQubit);
        } else {
                // need to get corresponding chunk of state vector from other rank
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                pairRank = getChunkPairId(rankIsUpper, multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                //printf("%d rank has pair rank: %d\n", multiQubit.rank, pairRank);
                // get corresponding values from my pair
		exchangeStateVectors(multiQubit, pairRank);
                // this rank's values are either in the upper of lower half of the block. sigmaX just replaces
		// this rank's values with pair values
		sigmaXDistributed(multiQubit, rotQubit,
			multiQubit.pairStateVec, // in
			multiQubit.stateVec); // out
        }
}

void controlledNot(MultiQubit multiQubit, const int controlQubit, const int rotQubit)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);
        QuESTAssert(controlQubit >= 0 && controlQubit < multiQubit.numQubits, 2, __func__);
        QuESTAssert(controlQubit != rotQubit, 3, __func__);

        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);

        // rank's chunk is in upper half of block 
        int rankIsUpper;
        int pairRank; // rank of corresponding chunk

        if (useLocalDataOnly){
                // all values required to update state vector lie in this rank
                controlledNotLocal(multiQubit, controlQubit, rotQubit);
        } else {
                // need to get corresponding chunk of state vector from other rank
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                pairRank = getChunkPairId(rankIsUpper, multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                //printf("%d rank has pair rank: %d\n", multiQubit.rank, pairRank);
                // get corresponding values from my pair
		exchangeStateVectors(multiQubit, pairRank);
                // this rank's values are either in the upper of lower half of the block. send values to controlledNot
                // in the correct order
                if (rankIsUpper){
                        controlledNotDistributed(multiQubit,controlQubit,rotQubit,
                                multiQubit.pairStateVec, //in
                                multiQubit.stateVec); //out
                } else {
                        controlledNotDistributed(multiQubit,controlQubit,rotQubit,
                                multiQubit.pairStateVec, //in
                                multiQubit.stateVec); //out
                }
        }
}

void sigmaY(MultiQubit multiQubit, const int rotQubit)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);

        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);

        // rank's chunk is in upper half of block 
        int rankIsUpper;
        int pairRank; // rank of corresponding chunk

        if (useLocalDataOnly){
                // all values required to update state vector lie in this rank
                sigmaYLocal(multiQubit, rotQubit);
        } else {
		//! fix -- put duplicate code (sigmaX, sigmaY) in seperate function
                // need to get corresponding chunk of state vector from other rank
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                pairRank = getChunkPairId(rankIsUpper, multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                //printf("%d rank has pair rank: %d\n", multiQubit.rank, pairRank);
                // get corresponding values from my pair
		exchangeStateVectors(multiQubit, pairRank);
                // this rank's values are either in the upper of lower half of the block. sigmaX just replaces
		// this rank's values with pair values
		sigmaYDistributed(multiQubit,rotQubit,
			multiQubit.pairStateVec, // in
			multiQubit.stateVec, // out
			rankIsUpper);
        }
}

void phaseGate(MultiQubit multiQubit, const int rotQubit, enum phaseGateType type)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);

        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);

        // rank's chunk is in upper half of block 
        int rankIsUpper;

        if (useLocalDataOnly){
                phaseGateLocal(multiQubit, rotQubit, type);
        } else {
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
		if (!rankIsUpper) phaseGateDistributed(multiQubit, rotQubit, type);
        }
}

void hadamard(MultiQubit multiQubit, const int rotQubit)
{
        QuESTAssert(rotQubit >= 0 && rotQubit < multiQubit.numQubits, 1, __func__);

        // flag to require memory exchange. 1: an entire block fits on one rank, 0: at most half a block fits on one rank
        int useLocalDataOnly = halfMatrixBlockFitsInChunk(multiQubit.numAmps, rotQubit);

        // rank's chunk is in upper half of block 
        int rankIsUpper;
        int pairRank; // rank of corresponding chunk

        if (useLocalDataOnly){
                // all values required to update state vector lie in this rank
                hadamardLocal(multiQubit, rotQubit);
        } else {
                // need to get corresponding chunk of state vector from other rank
                rankIsUpper = chunkIsUpper(multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                pairRank = getChunkPairId(rankIsUpper, multiQubit.chunkId, multiQubit.numAmps, rotQubit);
                //printf("%d rank has pair rank: %d\n", multiQubit.rank, pairRank);
                // get corresponding values from my pair
		exchangeStateVectors(multiQubit, pairRank);
                // this rank's values are either in the upper of lower half of the block. send values to hadamardDistributed
                // in the correct order
                if (rankIsUpper){
                        hadamardDistributed(multiQubit,rotQubit,
                                multiQubit.stateVec, //upper
                                multiQubit.pairStateVec, //lower
                                multiQubit.stateVec, rankIsUpper); //output
                } else {
                        hadamardDistributed(multiQubit,rotQubit,
                                multiQubit.pairStateVec, //upper
                                multiQubit.stateVec, //lower
                                multiQubit.stateVec, rankIsUpper); //output
                }
        }
}


/** Find chunks to skip when calculating probability of qubit being zero.
When calculating probability of a bit q being zero,
sum up 2^q values, then skip 2^q values, etc. This function finds if an entire chunk
is in the range of values to be skipped

@param[in] chunkId id of chunk in state vector
@param[in] chunkSize number of amps in chunk
@param[in] measureQubi qubit being measured
@return int -- 1: skip, 0: don't skip
*/

static int isChunkToSkipInFindPZero(int chunkId, long long int chunkSize, int measureQubit)
{
        long long int sizeHalfBlock = 1LL << (measureQubit);
        int numChunksToSkip = sizeHalfBlock/chunkSize;
        // calculate probability by summing over numChunksToSkip, then skipping numChunksToSkip, etc
        int bitToCheck = chunkId & numChunksToSkip;
        return bitToCheck;
}

REAL findProbabilityOfOutcome(MultiQubit multiQubit, const int measureQubit, int outcome)
{
    QuESTAssert(measureQubit >= 0 && measureQubit < multiQubit.numQubits, 2, __func__);

	REAL stateProb=0, totalStateProb=0;
	int skipValuesWithinRank = halfMatrixBlockFitsInChunk(multiQubit.numAmps, measureQubit);
	if (skipValuesWithinRank) {
		stateProb = findProbabilityOfZeroLocal(multiQubit, measureQubit);
	} else {
		if (!isChunkToSkipInFindPZero(multiQubit.chunkId, multiQubit.numAmps, measureQubit)){
			stateProb = findProbabilityOfZeroDistributed(multiQubit, measureQubit);
		} else stateProb = 0;
	}
	MPI_Allreduce(&stateProb, &totalStateProb, 1, MPI_QuEST_REAL, MPI_SUM, MPI_COMM_WORLD);
	if (outcome==1) totalStateProb = 1.0 - totalStateProb;
	return totalStateProb;
}


REAL collapseToOutcome(MultiQubit multiQubit, const int measureQubit, int outcome)
{
    QuESTAssert(measureQubit >= 0 && measureQubit < multiQubit.numQubits, 2, __func__);

	REAL totalStateProb=findProbabilityOfOutcome(multiQubit, measureQubit, outcome);
    QuESTAssert(fabs(totalStateProb>REAL_EPS), 8, __func__);

	int skipValuesWithinRank = halfMatrixBlockFitsInChunk(multiQubit.numAmps, measureQubit);
    if (skipValuesWithinRank) {
        collapseToOutcomeLocal(multiQubit, measureQubit, totalStateProb, outcome);
    } else {
        if (!isChunkToSkipInFindPZero(multiQubit.chunkId, multiQubit.numAmps, measureQubit)){
            // chunk has amps for q=0
            if (outcome==0) collapseToOutcomeDistributedRenorm(multiQubit, measureQubit, 
                    totalStateProb);
            else collapseToOutcomeDistributedSetZero(multiQubit, measureQubit);
        } else {
            // chunk has amps for q=1
            if (outcome==1) collapseToOutcomeDistributedRenorm(multiQubit, measureQubit, 
                    totalStateProb);
            else collapseToOutcomeDistributedSetZero(multiQubit, measureQubit);
        }
    }
	return totalStateProb;
}

void exitWithError(int errorCode, const char* func){
    printf("!!!\n");
    printf("QuEST Error in function %s: %s\n", func, errorCodes[errorCode]);
    printf("!!!\n");
    printf("exiting..\n");
    MPI_Abort(MPI_COMM_WORLD, errorCode);
}

void QuESTAssert(int isValid, int errorCode, const char* func){
    if (!isValid) exitWithError(errorCode, func);
}
