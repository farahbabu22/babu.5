#include <sys/time.h>
#include "parameters.h"

// For sharing memory and semaphores between enviornment
#define ftokPathName "/"

enum projID {shmProjID=4567, semProjID};


//process states
enum states { pREADY=1, pBLOCK, stTERM};

//request states
enum requestState { rACCEPT=0, rBLOCK, rWAIT, rDENY};

struct descriptorRequest {
	int id;					//ex. R1
	int val;				//ex. 2
	enum requestState state;
};

struct descriptor {
	int shareable;
	int max;		//maximum value
	int val;	//currently available
};

struct process {
	int	pid, id;	//process and OSS ID

	enum states processState;
	struct descriptorRequest request;	//current process request

	struct descriptor desc[descriptorCount];	//process resource descriptors
};

struct oss {
	struct timeval time;
	struct process procs[processSize];

	struct descriptor desc[descriptorCount];	//system resource descriptors
	int terminateFlag;
};
