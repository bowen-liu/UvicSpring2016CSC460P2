/***********************************************************************
  Kernel.h and Kernel.c contains the backend of the RTOS.
  It contains the underlying Kernel that process all requests coming in from OS syscalls.
  Most of Kernel's functions are not directly usable, but few are provided for the OS for convenience and for booting purposes.
  ***********************************************************************/

#ifndef KERNEL_H_
#define KERNEL_H_

#include <avr/io.h>
#include <avr/interrupt.h>
#include "os.h"

#ifdef DEBUG
#include "uart/uart.h"
#include <string.h>
#endif


//Global configurations
#define TICK_LENG 157			//The length of a tick = 10ms, using 16Mhz clock and /8 prescsaler

//Misc macros
#define Disable_Interrupt()		asm volatile ("cli"::)
#define Enable_Interrupt()		asm volatile ("sei"::)
  
//Definitions for potential errors the RTOS may come across
typedef enum error_codes
{
	NO_ERR  = 0,
	INVALID_KERNET_REQUEST_ERR,
	KERNEL_INACTIVE_ERR,
	MAX_PROCESS_ERR,
	PID_NOT_FOUND_ERR,
	SUSPEND_NONRUNNING_TASK_ERR,
	RESUME_NONSUSPENDED_TASK_ERR
} ERROR_TYPE;
  
typedef enum process_states 
{ 
   DEAD = 0, 
   READY, 
   RUNNING,
   SUSPENDED,
   SLEEPING 
} PROCESS_STATES;

typedef enum kernel_request_type 
{
   NONE = 0,
   CREATE,
   YIELD,
   TERMINATE,
   SUSPEND,
   RESUME,
   SLEEP
} KERNEL_REQUEST_TYPE;


/*Process descriptor for a task*/
typedef struct ProcessDescriptor 
{
   PID pid;									//An unique process ID for this task.
   PRIORITY pri;							//The priority of this task, from 0 (highest) to 10 (lowest).
   PROCESS_STATES state;					//What's the current state of this task?
   KERNEL_REQUEST_TYPE request;				//What the task want the kernel to do (when needed).
   volatile int request_arg;				//What value is needed for the specified kernel request.
   int arg;									//Initial argument for the task (if specified).
   unsigned char *sp;						//stack pointer into the "workSpace".
   unsigned char workSpace[WORKSPACE];		//Data memory allocated to this process.
   voidfuncptr  code;						//The function to be executed when this process is running.
} PD;


/*Context Switching functions defined in cswitch.c*/
extern void CSwitch();
extern void Enter_Kernel();	
extern void Exit_Kernel();


/*Kernel functions accessed by the OS*/
void OS_Init();
void OS_Start();
void Kernel_Create_Task(voidfuncptr f, PRIORITY py, int arg);
int findPIDByFuncPtr(voidfuncptr f);


/*Kernel variables accessed by the OS*/
extern volatile PD* Cp;
extern volatile unsigned char *KernelSp;
extern volatile unsigned char *CurrentSp;
extern volatile unsigned int last_PID;
extern volatile ERROR_TYPE err;
extern volatile unsigned int KernelActive;

#endif /* KERNEL_H_ */