#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>

#include "oss.h"

struct descriptor processStart, processRun, processRunTime, processDone;
static int optionVerbose = 0; //verbose mode

//variables for printing
static unsigned int requestCount = 0;
static unsigned int acceptCount = 0;
static unsigned int blockedCount = 0;
static unsigned int freeCount = 0;
static unsigned int releaseCountAll = 0;
static unsigned int denyCount = 0;
static unsigned int linesCount = 0;

//shared memory and semaphore ids
static int shmID = -1, semID = -1;
static struct oss *ossptr = NULL;

static int aliveFlag = 1;
static struct timeval clockTick; //the tick of our OSS system clock

static int blocked[processSize]; //blocked queue
static int blockedLength = 0;    //queue is empty at start

//Lock the shared region in signal safe way
static int ossSemWait()
{
  struct sembuf sop = {.sem_num = 0, .sem_flg = 0, .sem_op = -1};

  sigset_t mask, oldmask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &mask, &oldmask);

  if (semop(semID, &sop, 1) == -1)
  {
    perror("oss: semop error");
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    exit(EXIT_FAILURE);
  }

  sigprocmask(SIG_SETMASK, &oldmask, NULL);

  return 0;
}

//Signal to unlock the shared buffer
static int ossSemPost()
{
  struct sembuf sop = {.sem_num = 0, .sem_flg = 0, .sem_op = 1};

  sigset_t mask, oldmask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &mask, &oldmask);

  if (semop(semID, &sop, 1) == -1)
  {
    perror("oss: semop error");
    exit(EXIT_FAILURE);
  }

  sigprocmask(SIG_SETMASK, &oldmask, NULL);

  return 0;
}

//Add process to blocked queue
static int enqueueBlocked(const int processID)
{

  if (blockedLength < processSize)
  {
    blocked[blockedLength++] = processID;
    return 0;
  }
  else
  {
    return -1;
  }
}

//Remove a process from blocked queue
static int dequeueBlocked(const int index)
{
  int i;

  //check range
  if (index >= blockedLength)
  {
    return -1;
  }

  //get process at particular index
  const int processID = blocked[index];
  --blockedLength;

  //move rest of queue forward
  for (i = index; i < blockedLength; ++i)
  {
    blocked[i] = blocked[i + 1];
  }
  blocked[i] = -1; //clear last queue slot

  return processID;
}

static void descriptorsInit(struct descriptor sys[descriptorCount])
{
  int i;
  int sharedDesc = descriptorCount / (100 / sharedDescriptors);

  printf("Master Shared descriptor count %d\n", sharedDesc);
  linesCount++;

  //decide which descriptor will be shared
  while (sharedDesc > 0)
  {
    //generate random
    const int descriptorID = rand() % descriptorCount;

    if (sys[descriptorID].shareable == 0)
    {                                  //if its not shared yet
      sys[descriptorID].shareable = 1; //make it shared
      --sharedDesc;
    }
  }

  //fill descriptor value
  for (i = 0; i < descriptorCount; i++)
  {

    if (sys[i].shareable == 0)
    {
      sys[i].max = 1 + (rand() % descriptorValueMax);
    }
    else
    {
      sys[i].max = 1;
    }
    sys[i].val = sys[i].max;
  }
}

//Update the OSS time in safe way
static int ossTimeUpdate(struct timeval *update)
{
  struct timeval tv;

  if (ossSemWait() < 0)
  {
    return -1;
  }

  timeradd(&ossptr->time, update, &tv);
  ossptr->time = tv;

  return ossSemPost();
}

//ID to index
static int idToIndex(const int id)
{
  int i;
  for (i = 0; i < processSize; i++)
  {
    if (ossptr->procs[i].id == id)
    {
      return i;
    }
  }
  return -1;
}

//PID to index
static int pidToIndex(const int pid)
{
  int i;
  for (i = 0; i < processSize; i++)
  {
    if (ossptr->procs[i].pid == pid)
    {
      return i;
    }
  }
  return -1;
}

//Wait and clear all exited processes
static void ossWaitpid()
{
  pid_t pid;
  int status, idx;

  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
  {

    idx = pidToIndex(pid);
    if (idx == -1)
    {
      continue;
    }

    printf("Master Child %i processDone code %d at time %lu:%i\n",
           ossptr->procs[idx].id, WEXITSTATUS(status), ossptr->time.tv_sec, ossptr->time.tv_usec);
    linesCount++;

    //clear the processDone process
    bzero(&ossptr->procs[idx], sizeof(struct process));

    --processRun.val;
    if (++processDone.val >= processStart.max)
    {
      printf("Master All process exited. Stopping OSS\n");
      aliveFlag = 0;
    }
  }
}

static void ossAtExit()
{
  int i;

  if (ossptr)
  {
    //some user may be waiting on timer
    ossptr->time.tv_sec++;

    //deny all requests
    ossSemWait();
    for (i = 0; i < processSize; i++)
    {
      if ((ossptr->procs[i].request.state == rWAIT) ||
          (ossptr->procs[i].request.state == rBLOCK))
      {

        ossptr->procs[i].request.state = rDENY;
      }
    }
    ossSemPost();

    //wait for all procs to finish
    while (processRun.val > 0)
    {
      ossWaitpid();
    }
  }

  printf("Master exit at time %lu:%06ld\n", ossptr->time.tv_sec, ossptr->time.tv_usec);

  shmctl(shmID, IPC_RMID, NULL);
  semctl(semID, 0, IPC_RMID);
  shmdt(ossptr);

  exit(EXIT_SUCCESS);
}

//Find an unused process
static int findUnused(struct process *procs)
{
  int i;
  for (i = 0; i < processSize; i++)
  {
    if (procs[i].pid == 0)
    {
      return i;
    }
  }
  return -1;
}

//Spawn a user program
static int startProcess()
{
  char arg_id[5];

  const int idx = findUnused(ossptr->procs);
  if (idx == -1)
  {
    return -1;
  }
  struct process *proc = &ossptr->procs[idx];

  snprintf(arg_id, 5, "%d", idx);

  const pid_t pid = fork();
  if (pid == -1)
  {
    perror("oss: error in fork");
    return -1;
  }

  //child
  if (pid == 0)
  {
    execl("user_proc", "user_proc", arg_id, NULL);
    perror("oss: error in execl");
    exit(EXIT_FAILURE);

    //oss
  }
  else
  {
    proc->pid = pid;
    proc->id = processStart.val++;
    proc->processState = pREADY;

    ++processRun.val;

    printf("Master Generating process with PID %u at time %lu:%06ld\n", proc->id, ossptr->time.tv_sec, ossptr->time.tv_usec);
  }

  return 0;
}

//Attach the ossptr in shared memory
static int ossAttach()
{
  const unsigned short unlocked = 1;

  key_t k = ftok(ftokPathName, shmProjID);

  //shared memory
  const int flags = IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR;
  shmID = shmget(k, sizeof(struct oss), flags);
  if (shmID == -1)
  {
    perror("oss: error in shmget");
    return -1;
  }

  k = ftok(ftokPathName, semProjID);
  semID = semget(k, 1, flags);
  if (semID == -1)
  {
    perror("oss: error in semget");
    return -1;
  }

  //attach
  ossptr = (struct oss *)shmat(shmID, NULL, 0);
  if (ossptr == NULL)
  {
    perror("oss: error in shmat");
    return -1;
  }

  //clear the memory
  bzero(ossptr, sizeof(struct oss));

  //set semaphores to unlocked
  if (semctl(semID, 0, SETVAL, unlocked) == -1)
  {
    perror("oss: error in semctl");
    return -1;
  }

  return 0;
}

static void sigHandler(const int sig)
{

  switch (sig)
  {
  case SIGTERM:
  case SIGINT:
    printf("Master TERM/INT at time %lu:%06ld\n", ossptr->time.tv_sec, ossptr->time.tv_usec);
    aliveFlag = 0; //stop master loop
    break;

  case SIGALRM:
    printf("Master ALRM at time %lu:%06ld\n", ossptr->time.tv_sec, ossptr->time.tv_usec);
    aliveFlag = 0;
    break;

  case SIGCHLD:
    ossWaitpid();
    break;

  default:
    fprintf(stderr, "Error: Unknown signal received\n");
    break;
  }
}

//Unblock a process, and change its state
static int unblock(const int b, const enum requestState new_state)
{

  const int id = dequeueBlocked(b);
  const int idx = idToIndex(id);
  struct process *proc = &ossptr->procs[idx];

  ossSemWait();

  //update process and request states
  proc->processState = pREADY;
  proc->request.state = new_state;

  if (proc->request.state == rDENY)
  {
    denyCount++;
    printf("Master Denied process with PID %d waiting on R%d=%d at time %lu:%06ld\n",
           proc->id, proc->request.id, proc->request.val, ossptr->time.tv_sec, ossptr->time.tv_usec);
  }
  else
  {

    acceptCount++;

    //remove requested amount from system resources
    ossptr->desc[proc->request.id].val -= proc->request.val;
    proc->desc[proc->request.id].val += proc->request.val;
    proc->desc[proc->request.id].max -= proc->request.val;

    printf("Master Unblocked process with PID %d waiting on R%d=%d at time %lu:%06ld\n",
           proc->id, proc->request.id, proc->request.val, ossptr->time.tv_sec, ossptr->time.tv_usec);
  }
  ossSemPost();

  return 0;
}

//try to unblock processes in blocked queue
static int tryUnblock()
{
  int i, n = 0;
  for (i = 0; i < blockedLength; i++)
  {

    const int id = blocked[i];
    const int idx = idToIndex(id);
    struct process *proc = &ossptr->procs[idx];

    if (ossptr->desc[proc->request.id].val >= proc->request.val)
    { //if we have enough resource
      unblock(i, rACCEPT);
      n++; //one more unblocked process
    }
  }

  if (n == 0)
  { //if nobody was unblocked
    //every process is in the queue
    if ((blockedLength > 0) &&
        (blockedLength == processRun.val))
    {

      //unblock first to avoid deadlock
      unblock(0, rDENY);
    }
    else
    {
      return 0; //nobody to unblock
    }
  }

  return 1;
}

//List the processes in a deadlock
static int listDeadlocked(const int pdone[processSize])
{

  int i, have_deadlocked = 0;
  for (i = 0; i < processSize; i++)
  {
    if (!pdone[i])
    { //if process is not finished / done
      have_deadlocked = 1;
      break;
    }
  }

  if (optionVerbose && (have_deadlocked > 0))
  {
    printf("\tProcesses ");
    for (i = 0; i < processSize; i++)
    {
      if (!pdone[i])
      {
        printf("P%d ", ossptr->procs[i].id);
      }
    }
    printf("deadlocked.\n");
  }

  return have_deadlocked;
}

//Test for a deadlock for resource descriptors
static int detectDeadlock()
{

  int i, j, avail[descriptorCount], pdone[processSize];

  //at start all procs are not done
  for (i = 0; i < processSize; i++)
  {
    pdone[i] = 0;
  }

  //sum the available resources
  for (i = 0; i < descriptorCount; i++)
  {
    avail[i] = ossptr->desc[i].val;
  }

  i = 0;
  while (i != processSize)
  {

    for (i = 0; i < processSize; i++)
    {

      struct process *proc = &ossptr->procs[i];
      if (proc->pid == 0)
      {
        pdone[i] = 1;
      }

      if (pdone[i] == 1)
      {
        continue;
      }

      //check if process can finish
      int can_complete = 1;
      for (j = 0; j < descriptorCount; j++)
      {
        //if process need is more than system available resource
        if (ossptr->desc[j].val < (proc->desc[j].max - proc->desc[j].val))
        {
          can_complete = 0; //can't complete
          break;
        }
      }

      //if user is releasing or request is accepted or can complete
      if ((proc->request.val < 0) ||
          (proc->request.state != rWAIT) ||
          can_complete)
      {

        //mark it as complete
        pdone[i] = 1;

        //add its resource to available
        for (j = 0; j < descriptorCount; j++)
        {
          avail[j] += proc->desc[j].val;
        }
        break;
      }
    }
  }

  //check if we have a deadlock
  return listDeadlocked(pdone);
}

static int onRequest(struct process *proc)
{

  //process the request
  requestCount++;

  if (proc->request.val < 0)
  { //if user wants to release

    proc->request.state = rACCEPT; //request is accepted
    //return resource to system
    ossptr->desc[proc->request.id].val += -1 * proc->request.val;
    proc->desc[proc->request.id].val += proc->request.val;

    printf("Master has acknowledged Process P%u releasing R%d:%d at time %lu:%06ld\n",
           proc->id, proc->request.id, -1 * proc->request.val, ossptr->time.tv_sec, ossptr->time.tv_usec);

    freeCount++;

    //id=-1, means user is returning all of its resources
  }
  else if (proc->request.id == -1)
  {
    int i;

    proc->request.state = rACCEPT;

    printf("Master has acknowledged Process P%u releasing all resources: ", proc->id);

    for (i = 0; i < descriptorCount; i++)
    {
      if (proc->desc[i].val > 0)
      {
        printf("R%d:%d ", i, proc->desc[i].val);

        //return resource to system
        ossptr->desc[i].val += proc->desc[i].val;
        proc->desc[i].val = 0;
      }
    }
    printf("\n");
    releaseCountAll++;

    //user wants to request a resource
  }
  else if (ossptr->desc[proc->request.id].val >= proc->request.val)
  { //if we have enough resource

    printf("Master running deadlock detection at time %lu:%06ld\n", ossptr->time.tv_sec, ossptr->time.tv_usec);
    linesCount++;

    //check if we are in deadlock, after request
    if (detectDeadlock())
    {

      printf("Unsafe state after granting request; request not granted\n");
      linesCount++;

      proc->request.state = rDENY;
      denyCount++;
    }
    else
    { //no deadlock, we accept

      printf("\tSafe state after granting request\n");
      printf("\tMaster granting P%d request R%d:%d at time %lu:%06ld\n", proc->id, proc->request.id, proc->request.val,
             ossptr->time.tv_sec, ossptr->time.tv_usec);
      linesCount += 2;

      //update system and user resource descriptors
      ossptr->desc[proc->request.id].val -= proc->request.val;
      proc->desc[proc->request.id].val += proc->request.val;

      //reduce user resource need
      proc->desc[proc->request.id].max -= proc->request.val;

      proc->request.state = rACCEPT;
      acceptCount++;
    }
  }
  else
  { //not enough system resource, user gets blocked

    if (enqueueBlocked(proc->id) == 0)
    {
      proc->request.state = rBLOCK;
      blockedCount++;
      printf("\tP%d added to wait queue, waiting on R%d=%d\n", proc->id, proc->request.id, proc->request.val);
    }
    else
    {
      proc->request.state = rDENY;
      denyCount++;
      printf("\tP%d wans't added to wait queue, queue is full\n", proc->id);
    }
    linesCount++;
  }
  return 0;
}

static int processRequests()
{
  int i;
  sigset_t mask, oldmask;
  struct timeval tv;

  //block child signals, while we process requests
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &mask, &oldmask);

  //lock the shared region
  ossSemWait();

  for (i = 0; i < processSize; i++)
  {

    struct process *proc = &ossptr->procs[i];
    //if user have made a request
    if (proc->request.state == rWAIT)
    {
      onRequest(proc);
    }
  }
  ossSemPost();

  //allow the child signals, again
  sigprocmask(SIG_SETMASK, &oldmask, NULL);

  //update the timer, with dispatch time
  tv.tv_sec = 0;
  tv.tv_usec = rand() % 100;
  if (ossTimeUpdate(&tv) < 0)
  {
    return -1;
  }
  printf("Master total time this dispatching was %06ld nanoseconds at time %lu:%06ld\n", tv.tv_usec, ossptr->time.tv_sec, ossptr->time.tv_usec);
  linesCount++;

  return 0;
}

static void listSystemResources()
{
  int i, j;

  printf("System available resources at time %li.%06ld\n", ossptr->time.tv_sec, ossptr->time.tv_usec);
  linesCount++;

  //print resource title
  printf("    ");
  for (i = 0; i < descriptorCount; i++)
  {
    printf("R%2d ", i);
  }
  printf("\n");

  //print max
  printf("MAX ");
  for (i = 0; i < descriptorCount; i++)
  {
    printf("%*d ", 3, ossptr->desc[i].max);
  }
  printf("\n");

  //print available now
  printf("NOW ");
  for (i = 0; i < descriptorCount; i++)
  {
    printf("%3d ", ossptr->desc[i].val);
  }
  printf("\n");
  linesCount += 3;

  //print what process have
  for (i = 0; i < processSize; i++)
  {
    struct process *proc = &ossptr->procs[i];
    if (proc->pid > 0)
    {
      printf("P%2d ", proc->id);

      for (j = 0; j < descriptorCount; j++)
      {
        printf("%3d ", proc->desc[j].val);
      }
      printf("\n");
    }
  }
  linesCount += processSize;
}

static void listRequstedResource()
{

  int i;
  printf("Master resource requests at time %li.%06ld\n", ossptr->time.tv_sec, ossptr->time.tv_usec);

  for (i = 0; i < processSize; i++)
  {
    struct process *proc = &ossptr->procs[i];

    if ((proc->pid > 0) &&
        (proc->request.val != 0))
    {

      printf("P%d: R%d:%d\n", proc->id, proc->request.id, proc->request.val);
      linesCount++;
    }
  }
}

int main(void)
{

  struct timeval next_start = {.tv_sec = 1, .tv_usec = 5000};

  atexit(ossAtExit);

  //default options
  processStart.val = 0;
  processStart.max = processMaximum;

  processDone.val = 0;
  processDone.max = processMaximum;

  processRun.val = 0;
  processRun.max = processSize;

  processRunTime.val = 0;
  processRunTime.max = maxRuntime;

  stdout = freopen("output.log", "w", stdout);

  if (ossAttach() < 0)
  {
    ossAtExit();
  }

  //our clock step of 100 ns
  clockTick.tv_sec = 0;
  clockTick.tv_usec = 100;

  //ignore signals to avoid interrupts in msgrcv
  signal(SIGINT, sigHandler);
  signal(SIGTERM, sigHandler);
  signal(SIGCHLD, sigHandler);
  signal(SIGALRM, sigHandler);

  alarm(processRunTime.max);

  //allocate and setup resource descriptors
  descriptorsInit(ossptr->desc);

  //loop until all processes are done
  while (aliveFlag)
  {

    //update our simulated clock
    if (ossTimeUpdate(&clockTick) < 0)
    {
      break;
    }

    //if its time to start a new process
    if (timercmp(&ossptr->time, &next_start, >=) != 0)
    {

      //if we can start another process
      if (processStart.val < processStart.max)
      {
        startProcess();

        //next fork will be after 0.5 sec
        next_start.tv_sec = ossptr->time.tv_sec;
        next_start.tv_usec = ossptr->time.tv_usec + (rand() % 5000);
      }
    }

    //chcek who made requests
    processRequests();
    //check the blocked processes
    tryUnblock();

    //output resource maps
    if ((optionVerbose == 1) && ((requestCount % 20) == 1))
    {
      listSystemResources();
      listRequstedResource();
      fflush(stdout);
    }

    //check line limit
    if (linesCount > lineLimit)
    {
      stdout = freopen("/dev/null", "w", stdout);
      linesCount = 0;
    }
  }

  //restore the output, if it was sent to /dev/null
  stdout = freopen("output.log", "a", stdout);

  //list system resources at end
  listSystemResources();

  printf("Time taken: %lu:%06ld\n", ossptr->time.tv_sec, ossptr->time.tv_usec);

  printf("Requests: %d\n", requestCount);
  printf("Accepted: %d\n", acceptCount);
  printf("Blocked: %d\n", blockedCount);

  printf("Freed: %d\n", freeCount);
  printf("Freed All: %d\n", releaseCountAll);
  printf("Denied (deadlocks): %d\n", denyCount);
  printf("Grants per process: %.1f\n", (float)requestCount / processStart.val);

  return 0;
}
