#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include "os.h"
#include "kernel.h"

#define DEBUG

#ifdef DEBUG
	#include "uart/uart.h"
	#include <string.h>
#endif 

extern void CSwitch();
extern void Exit_Kernel();
extern void Enter_Kernel();

volatile static PD Process[MAXTHREAD];					//Contains the process descriptor for all tasks, regardless of their current state.

volatile static PD* Cp;							//The process descriptor of the currently RUNNING task. CP is used to pass information from OS calls to the kernel telling it what to do.
volatile unsigned char *KernelSp;				//Pointer to the Kernel's own stack location.
volatile unsigned char *CurrentSp;				//Pointer to the stack location of the current running task. Used for saving into PD during ctxswitch.
volatile static unsigned int NextP;				//Which task in the process queue to dispatch next.
volatile static unsigned int KernelActive;		//Indicates if kernel has been initialzied by OS_Start().
volatile static unsigned int Tasks;				//Number of tasks created so far .
volatile static unsigned int last_PID = 0;		//Highest PID value created so far.
volatile static ERROR_TYPE err = NO_ERR;		//Error code for the previous kernel operation (if any)


/************************************************************************/
/*						    KERNEL HELPERS                              */
/************************************************************************/

/*Returns the pointer of a process descriptor in the global process list, by searching for its PID*/
PD* findProcessByPID(int pid)
{
	int i;
	
	for(i=0; i<MAXTHREAD; i++)
	{
		if (Process[i].pid == pid)
			return &(Process[i]);
	}
	
	//No process with such PID
	return NULL;
}
/*Returns the PID associated with a function's memory address*/
int findPIDByFuncPtr(voidfuncptr f)
{
	int i;
	
	for(i=0; i<MAXTHREAD; i++)
	{
		if (Process[i].code == f)
			return Process[i].pid;
	}
	
	//No process with such PID
	return -1;
}

/************************************************************************/
/*                           KERNEL FUNCTIONS                           */
/************************************************************************/

//This ISR processes all tasks that are currently sleeping, waking them up when their tick expires
ISR(TIMER1_COMPA_vect)
{
	int i;
	int temp;
	for(i=0; i<MAXTHREAD; i++)
	{
		//Ignore any processes that's not SLEEPING
		if(Process[i].state == SLEEPING)
		{
			//If the current sleeping task's tick count expires, put it back into its READY state
			if(--Process[i].request_arg <= 0)
				Process[i].state = READY;
		}
	}
}

/* This internal kernel function is a part of the "scheduler". It chooses the next task to run, i.e., Cp. */
static void Dispatch()
{
   int i = 0;
   
   //Find the next READY task by iterating through the process list
   while(Process[NextP].state != READY) 
   {
      NextP = (NextP + 1) % MAXTHREAD;
	  i++;
	  
	  //Not a single task is ready. We'll temporarily re-enable interrupt in case if one or more task is waiting on events/interrupts or sleeping
	  if(i > MAXTHREAD) Enable_Interrupt();
   }
   
   //Now that we have a ready task, interrupts must be disabled for the kernel to function properly again.
   Disable_Interrupt();
	
   //Load the task's process descriptor into Cp
   Cp = &(Process[NextP]);
   CurrentSp = Cp->sp;
   Cp->state = RUNNING;
	
	//Increment NextP so the next dispatch will not run the same process (unless everything else isn't ready)
   NextP = (NextP + 1) % MAXTHREAD;
}

/* Handles all low level operations for creating a new task */
static void Kernel_Create_Task(voidfuncptr f, PRIORITY py, int arg)
{
	int x;
	unsigned char *sp;
	PD *p;

	#ifdef DEBUG
	int counter = 0;
	#endif
	
	//Make sure the system can still have enough resources to create more tasks
	if (Tasks == MAXTHREAD)
	{
		err = MAX_PROCESS_ERR;
		return;
	}

	//Find a dead or empty PD slot to allocate our new task
	for (x = 0; x < MAXTHREAD; x++)
	if (Process[x].state == DEAD) break;
	
	++Tasks;
	p = &(Process[x]);
	
	/*The code below was agglomerated from Kernel_Create_Task_At;*/
	
	//Initializing the workspace memory for the new task
	sp = (unsigned char *) &(p->workSpace[WORKSPACE-1]);
	memset(&(p->workSpace),0,WORKSPACE);

	//Store terminate at the bottom of stack to protect against stack underrun.
	*(unsigned char *)sp-- = ((unsigned int)Task_Terminate) & 0xff;
	*(unsigned char *)sp-- = (((unsigned int)Task_Terminate) >> 8) & 0xff;
	*(unsigned char *)sp-- = 0x00;

	//Place return address of function at bottom of stack
	*(unsigned char *)sp-- = ((unsigned int)f) & 0xff;
	*(unsigned char *)sp-- = (((unsigned int)f) >> 8) & 0xff;
	*(unsigned char *)sp-- = 0x00;

	//Allocate the stack with enough memory spaces to save the registers needed for ctxswitch
	#ifdef DEBUG
	 //Fill stack with initial values for development debugging
	 for (counter = 0; counter < 34; counter++)
	 {
		 *(unsigned char *)sp-- = counter;
	 }
	#else
	 //Place stack pointer at top of stack
	 sp = sp - 34;
	#endif
	
	//Build the process descriptor for the new task
	p->pid = ++last_PID;
	p->pri = py;
	p->arg = arg;
	p->request = NONE;
	p->state = READY;
	p->sp = sp;					/* stack pointer into the "workSpace" */
	p->code = f;				/* function to be executed as a task */
	
	//No errors occured
	err = NO_ERR;
}

/*TODO: Check for mutex ownership. If PID owns any mutex, ignore this request*/
static void Kernel_Suspend_Task() 
{
	//Finds the process descriptor for the specified PID
	PD* p = findProcessByPID(Cp->request_arg);
	
	//Ensure the PID specified in the PD currently exists in the global process list
	if(p == NULL)
	{
		#ifdef DEBUG
			printf("Kernel_Suspend_Task: PID not found in global process list!\n");
		#endif
		err = PID_NOT_FOUND_ERR;
		return;
	}
	
	//Ensure the task is currently in the READY state
	if(p->state != READY)
	{
		#ifdef DEBUG
		printf("Kernel_Suspend_Task: Trying to suspend a task that's not READY!\n");
		#endif
		err = SUSPEND_NONRUNNING_TASK_ERR;
		return;
	}
	
	p->state = SUSPENDED;
	err = NO_ERR;
}

static void Kernel_Resume_Task()
{
	//Finds the process descriptor for the specified PID
	PD* p = findProcessByPID(Cp->request_arg);
	
	//Ensure the PID specified in the PD currently exists in the global process list
	if(p == NULL)
	{
		#ifdef DEBUG
			printf("Kernel_Resume_Task: PID not found in global process list!\n");
		#endif
		err = PID_NOT_FOUND_ERR;
		return;
	}
	
	//Ensure the task is currently in the SUSPENDED state
	if(p->state != SUSPENDED)
	{
		#ifdef DEBUG
		printf("Kernel_Resume_Task: Trying to resume a task that's not SUSPENDED!\n");
		#endif
		err = RESUME_NONSUSPENDED_TASK_ERR;
		return;
	}
	
	p->state = READY;
	err = NO_ERR;
}

/**
  * This internal kernel function is the "main" driving loop of this full-served
  * model architecture. Basically, on OS_Start(), the kernel repeatedly
  * requests the next user task's next system call and then invokes the
  * corresponding kernel function on its behalf.
  *
  * This is the main loop of our kernel, called by OS_Start().
  */
static void Next_Kernel_Request() 
{
	Dispatch();	//Select an initial task to run

	//After OS initialization, this will be kernel's main loop
	while(1) 
	{
		//Clears the process' request fields
		Cp->request = NONE;
		//Cp->request_arg is not reset, because task_sleep uses it to keep track of remaining ticks

		//Load the current task's stack pointer and switch to its context
		CurrentSp = Cp->sp;
		Exit_Kernel();

		/* if this task makes a system call, it will return to here! */

		//Save the current task's stack pointer and proceed to handle its request
		Cp->sp = CurrentSp;

		switch(Cp->request)
		{
			case CREATE:
			Kernel_Create_Task(Cp->code, Cp->pri, Cp->arg);
			break;
			
			case TERMINATE:
			Cp->state = DEAD;			//Mark the task as DEAD so its resources will be recycled later when new tasks are created
			Dispatch();
			break;
		   
			case SUSPEND:
			Kernel_Suspend_Task();
			break;
			
			case RESUME:
			Kernel_Resume_Task();
			break;
			
			case SLEEP:
			Cp->state = SLEEPING;
			Dispatch();					
			break;
		   
			case YIELD:
			case NONE:					// NONE could be caused by a timer interrupt
			Cp->state = READY;
			Dispatch();
			break;
       
			//Invalid request code, just ignore
			default:
				err = INVALID_KERNET_REQUEST_ERR;
			break;
       }
    } 
}



/************************************************************************/
/*						   RTOS API FUNCTIONS                           */
/************************************************************************/

/*Sets up the timer needed for task_sleep*/
void Timer_init()
{
	/*Timer1 is configured for the task*/
	
	//Use Prescaler = 1024
	TCCR1B |= (1<<CS12)|(1<<CS10);
	TCCR1B &= ~(1<<CS11);

	//Use CTC mode (mode 4)
	TCCR1B |= (1<<WGM12);
	TCCR1B &= ~((1<<WGM13)|(1<<WGM11)|(1<<WGM10));
	
	OCR1A = TICK_LENG;			//Set timer top comparison value to ~10ms
	TCNT1 = 0;					//Load initial value for timer
	TIMSK1 |= (1<<OCIE1A);      //enable match for OCR1A interrupt       
}

/*This function initializes the RTOS and must be called before any othersystem calls.*/
void OS_Init() 
{
   int x;

   Tasks = 0;
   KernelActive = 0;
   NextP = 0;
   
	//Reminder: Clear the memory for the task on creation.
  
   for (x = 0; x < MAXTHREAD; x++) {
      memset(&(Process[x]), 0, sizeof(PD));
      Process[x].state = DEAD;
   }
}

/* This function starts the RTOS after creating a few tasks.*/
void OS_Start() 
{   
   if ( (! KernelActive) && (Tasks > 0)) 
   {
       Disable_Interrupt();
	   
	   /*Initialize and start Timer needed for sleep*/
	   Timer_init();
	   
      /* we may have to initialize the interrupt vector for Enter_Kernel() here. */

      /* here we go...  */
      KernelActive = 1;
      Next_Kernel_Request();
      /* NEVER RETURNS!!! */
   }
}

/* OS call to create a new task */
PID Task_Create(voidfuncptr f, PRIORITY py, int arg)
{
   //Run the task creation through kernel if it's running already
   if (KernelActive) 
   {
     Disable_Interrupt();
	 
	 //Fill in the parameters for the new task into CP
	 Cp->pri = py;
	 Cp->arg = arg;
     Cp->request = CREATE;
     Cp->code = f;

     Enter_Kernel();
   } 
   else 
	   Kernel_Create_Task(f,py,arg);		//If kernel hasn't started yet, manually create the task
   
   //Return zero as PID if the task creation process gave errors. Note that the smallest valid PID is 1
   if (err == MAX_PROCESS_ERR)
   {
		#ifdef DEBUG
			printf("Task_Create: Failed to create task. The system is at its process threshold.\n");
		#endif
		return 0;
   }
   
   return last_PID;
}

/* The calling task terminates itself. */
/*TODO: CLEAN UP EVENTS AND MUTEXES*/
void Task_Terminate()
{
	if(!KernelActive){
		err = KERNEL_INACTIVE_ERR;
		return;
	}

	Disable_Interrupt();
	Cp -> request = TERMINATE;
	Enter_Kernel();			
}

/* The calling task gives up its share of the processor voluntarily. Previously Task_Next() */
void Task_Yield() 
{
	if(!KernelActive){
		err = KERNEL_INACTIVE_ERR;
		return;
	}

    Disable_Interrupt();
    Cp ->request = YIELD;
    Enter_Kernel();
}

int Task_GetArg()
{
	return 0;
}

void Task_Suspend(PID p)
{
	if(!KernelActive){
		err = KERNEL_INACTIVE_ERR;
		return;
	}
	Disable_Interrupt();
	
	//Sets up the kernel request fields in the PD for this task
	Cp->request = SUSPEND;
	Cp->request_arg = p;
	//printf("SUSPENDING: %u\n", Cp->request_arg);
	Enter_Kernel();
}

void Task_Resume(PID p)
{
	if(!KernelActive){
		err = KERNEL_INACTIVE_ERR;
		return;
	}
	Disable_Interrupt();
	
	//Sets up the kernel request fields in the PD for this task
	Cp->request = RESUME;
	Cp->request_arg = p;
	//printf("RESUMING: %u\n", Cp->request_arg);
	Enter_Kernel();
}

void Task_Sleep(int t)
{
	if(!KernelActive){
		err = KERNEL_INACTIVE_ERR;
		return;
	}
	Disable_Interrupt();
	
	Cp->request_arg = t;
	Cp->request = SLEEP;

	//printf("%u ticks\n", Cp->request_arg);
	Enter_Kernel();
}


/************************************************************************/
/*							CODE FOR TESTING                            */
/************************************************************************/

#define LED_PIN_MASK 0x80			//Pin 13 = PB7

void testSetup()
{
	DDRB = LED_PIN_MASK;			//Set pin 13 as output
}

void Ping()
{
	for(;;)
	{
		PORTB |= LED_PIN_MASK;		//Turn on onboard LED
		printf("PING!\n");
		//_delay_ms(100);
		Task_Sleep(10);
		Task_Yield();
	}
}

void Pong()
{
	for(;;)
	{
		PORTB &= ~LED_PIN_MASK;		//Turn off onboard LED
		printf("PONG!\n");
		//_delay_ms(100);
		Task_Sleep(10);
		Task_Yield();
	}
}

void suspend_pong()
{
	for(;;)
	{
		//_delay_ms(1000);
		Task_Sleep(10);
		printf("SUSPENDING PONG!\n");
		Task_Suspend(findPIDByFuncPtr(Pong));
		Task_Yield();
		
		//_delay_ms(1000);
		Task_Sleep(10);
		printf("RESUMING PONG!\n");
		Task_Resume(findPIDByFuncPtr(Pong));
		Task_Yield();
	}
	
}

void main() 
{
   //Enable STDIN/OUT to UART redirection for debugging
   #ifdef DEBUG
	uart_init();
	uart_setredir();
	printf("STDOUT->UART!\n");
   #endif  
   
   testSetup();
   
   OS_Init();
   Task_Create(Ping, 10, 210);
   Task_Create(Pong, 10, 205);
   Task_Create(suspend_pong, 10, 0);
   OS_Start();
   
}

