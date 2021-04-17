#include <string.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "oss.h"

static int shmID = -1, semID = -1; //memory and msg queue identifiers
static struct oss *ossptr = NULL;  //shared memory pointer
static struct process *user = NULL;

static int descriptorRelease(const struct descriptor sys[descriptorCount])
{
	int i, len = 0;
	int list[descriptorCount];

	//see what we have
	for (i = 0; i < descriptorCount; i++)
	{
		if (sys[i].val > 0)
		{
			list[len++] = i; //store desc id
		}
	}
	//return random descriptor ID or -1, if list is empty
	return (len == 0) ? -1 : list[rand() % len];
}

//Select one request at random
static int descriptorRequest(const struct descriptor sys[descriptorCount])
{
	int i, len = 0, list[descriptorCount];

	//see what we have
	for (i = 0; i < descriptorCount; i++)
	{
		if (sys[i].max > 0)
		{
			list[len++] = i; //store desc id
		}
	}
	//return random descriptor ID or -1, if list is empty
	return (len == 0) ? -1 : list[rand() % len];
}

//Generate random descriptor need[], using system as maximum value
static void descriptorGenerate(struct descriptor proc[descriptorCount], struct descriptor sys[descriptorCount])
{
	int i;
	for (i = 0; i < descriptorCount; i++)
	{
		if (sys[i].max <= 1)
		{
			proc[i].max = sys[i].max;
		}
		else
		{
			proc[i].max = 1 + (rand() % (sys[i].max - 1));
		}
	}
}

//Attach to OSS shared structures ( memory and semaphore)
static int ossAttach()
{

	key_t k = ftok(ftokPathName, shmProjID);

	shmID = shmget(k, 0, 0);
	if (shmID == -1)
	{
		perror("user_proc: shmget");
		return -1;
	}

	k = ftok(ftokPathName, semProjID);
	semID = semget(k, 0, 0);
	if (semID == -1)
	{
		perror("user_proc: semget");
		return -1;
	}

	//attach
	ossptr = (struct oss *)shmat(shmID, NULL, 0);
	if (ossptr == NULL)
	{
		perror("user_proc: shmat");
		return -1;
	}

	return 0;
}

static int ossSemWait()
{
	struct sembuf sop = {.sem_num = 0, .sem_flg = 0, .sem_op = -1};

	if (semop(semID, &sop, 1) == -1)
	{
		perror("user_proc: semop");
		return -1;
	}
	return 0;
}

//Unlock critical section
static int ossSemPost()
{
	struct sembuf sop = {.sem_num = 0, .sem_flg = 0, .sem_op = 1};

	if (semop(semID, &sop, 1) == -1)
	{
		perror("user_proc: semop");
		return -1;
	}
	return 0;
}

//Wait for a request, busy waiting on the semaphore
static int waitRequest(struct descriptorRequest *request)
{
	while ((request->state == rWAIT) ||
		   (request->state == rBLOCK))
	{

		if (ossSemPost() < 0)
		{
			return -1;
		}
		usleep(250);

		if (ossSemWait() < 0)
		{
			return -1;
		}
	}

	if (request->state == rDENY)
	{
		return -1;
	}

	return 0;
}

static int requestAction()
{
	int desc_id;
	static int max_prob = 100;

	//decide between request(0), release(1)
	int action = ((rand() % max_prob) < B) ? 0 : 1;

	switch (action)
	{
	case 0: //release
		desc_id = descriptorRelease(user->desc);
		if (desc_id == -1)
		{

			action = 1;
			desc_id = descriptorRequest(user->desc);
			if (desc_id == -1)
			{
				return -1;
			}
		}
		break;

	case 1: //request
		desc_id = descriptorRequest(user->desc);
		if (desc_id == -1)
		{
			max_prob = B;
			//r = 0;	//no stop
			action = 0;
			desc_id = descriptorRelease(user->desc);
			if (desc_id == -1)
			{
				return -1;
			}
		}
		break;
	}

	user->request.id = desc_id;
	user->request.val = (action == 0)
							? -1 * user->desc[desc_id].val
							: user->desc[desc_id].max;
	user->request.state = rWAIT;

	return waitRequest(&user->request);
}

int main(const int argc, char *const argv[])
{

	if (argc != 2)
	{
		fprintf(stderr, "Usage: ./user index\n");
		return EXIT_FAILURE;
	}

	if (ossAttach() < 0)
	{
		fprintf(stderr, "Error: Failed to attach\n");
		return EXIT_FAILURE;
	}
	user = &ossptr->procs[atoi(argv[1])];

	srand(time(NULL));

	//generate our resource descriptor need
	descriptorGenerate(user->desc, ossptr->desc);

	struct timeval tstep, tcheck;
	//next terminate check
	tstep.tv_sec = 0;
	tstep.tv_usec = termCheckPeriod;

	//copy shared timer to end and sub with tsteprement
	ossSemWait();
	timeradd(&tstep, &ossptr->time, &tcheck);
	ossSemPost();

	int stop = 0;
	while (!stop)
	{

		if (ossSemWait() < 0)
		{
			break;
		}

		//if its time for us to check termination status
		if (timercmp(&ossptr->time, &tcheck, >))
		{

			//update timer for next check
			tstep.tv_usec = termCheckPeriod;
			timeradd(&tstep, &ossptr->time, &tcheck);

			stop = ossptr->terminateFlag;
		}
		else
		{
			stop = requestAction();
		}

		if (ossSemPost() < 0)
		{
			break;
		}
	}

	//release all
	ossSemWait();
	user->request.id = -1;
	user->request.val = 0;
	user->request.state = rWAIT;
	waitRequest(&user->request);
	ossSemPost();

	shmdt(ossptr);
	return EXIT_SUCCESS;
}
