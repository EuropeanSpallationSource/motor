/*
FILENAME...	drvMM4000.c
USAGE...	Motor record driver level support for Newport MM4000.

Version:	$Revision: 1.1 $
Modified By:	$Author: sluiter $
Last Modified:	$Date: 2000-02-08 22:18:53 $
*/

/*
 *      Original Author: Mark Rivers
 *      Date: 10/20/97
 *	Current Author: Ron Sluiter
 *
 *      Experimental Physics and Industrial Control System (EPICS)
 *
 *      Copyright 1991, the Regents of the University of California,
 *      and the University of Chicago Board of Governors.
 *
 *      This software was produced under  U.S. Government contracts:
 *      (W-7405-ENG-36) at the Los Alamos National Laboratory,
 *      and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *      Initial development by:
 *	      The Controls and Automation Group (AT-8)
 *	      Ground Test Accelerator
 *	      Accelerator Technology Division
 *	      Los Alamos National Laboratory
 *
 *      Co-developed with
 *	      The Controls and Computing Group
 *	      Accelerator Systems Division
 *	      Advanced Photon Source
 *	      Argonne National Laboratory
 *
 * Modification Log:
 * -----------------
 * .01  10-20-97	mlr     initialized from drvOms58
 * .02  10-30-97        mlr     Replaced driver calls with gpipIO functions
 * .03  10-30-98        mlr     Minor code cleanup, improved formatting
 * .04  02-01-99        mlr     Added temporary fix to delay reading motor
 *                              positions at the end of a move.
 * .05  10-13-99	rls	modified for standardized motor record.
 */


#include	<vxWorks.h>
#include	<stdioLib.h>
#include	<sysLib.h>
#include	<string.h>
#include	<taskLib.h>
#include	<math.h>
#include        <rngLib.h>
#include	<alarm.h>
#include	<dbDefs.h>
#include	<dbAccess.h>
#include	<fast_lock.h>
#include	<recSup.h>
#include	<devSup.h>
#include        <errMdef.h>
#include	<logLib.h>

#include	"motor.h"
#include	"drvMMCom.h"
#include        "gpibIO.h"
#include        "serialIO.h"

#define STATIC static

#define READ_RESOLUTION "TU;\r"
#define READ_STATUS     "MS;\r"
#define READ_POSITION   "TP;\r"
#define STOP_ALL        "ST;\r"
#define MOTOR_ON        "MO;\r"
#define GET_IDENT       "VE;\r"

#define INPUT_TERMINATOR  '\r'

/* Status byte bits */
#define M_AXIS_MOVING     0x01
#define M_MOTOR_POWER     0x02
#define M_MOTOR_DIRECTION 0x04
#define M_PLUS_LIMIT      0x08
#define M_MINUS_LIMIT     0x10
#define M_HOME_SIGNAL     0x20

#define MM4000_NUM_CARDS	4
#define BUFF_SIZE 100       /* Maximum length of string to/from MM4000 */

/*----------------debugging-----------------*/
#ifdef	DEBUG
    #define Debug(l, f, args...) { if(l<=drvMM4000debug) printf(f,## args); }
#else
    #define Debug(l, f, args...)
#endif

/* --- Local data. --- */
int MM4000_num_cards = 0;
volatile int drvMM4000debug = 0;

/* Local data required for every driver; see "motordrvComCode.h" */
#include	"motordrvComCode.h"

/* This is a temporary fix to introduce a delayed reading of the motor
 * position after a move completes
 */
volatile double drvMM4000ReadbackDelay = 0.;


/*----------------functions-----------------*/
STATIC int recv_mess(int, char *, int);
STATIC int send_mess(int card, char const *com, char c);
STATIC void start_status(int card);
STATIC int set_status(int card, int signal);
static long report(int level);
static long init();
STATIC int motor_init();
STATIC void query_done(int, int, struct mess_node *);

/*----------------functions-----------------*/

struct driver_table MM4000_access =
{
    motor_init,
    motor_send,
    motor_free,
    motor_card_info,
    motor_axis_info,
    &mess_queue,
    &queue_lock,
    &free_list,
    &freelist_lock,
    &motor_sem,
    &motor_state,
    &total_cards,
    &any_motor_in_motion,
    send_mess,
    recv_mess,
    set_status,
    query_done,
    start_status,
    &initialized
};

struct
{
    long number;
#ifdef __cplusplus
    long (*report) (int);
    long (*init) (void);
#else
    DRVSUPFUN report;
    DRVSUPFUN init;
#endif
} drvMM4000 = {2, report, init};


/*********************************************************
 * Print out driver status report
 *********************************************************/
static long report(int level)
{
    int card;

    if (MM4000_num_cards <=0)
	printf("    No MM4000 controllers configured.\n");
    else
    {
	for (card = 0; card < MM4000_num_cards; card++)
	{
	    struct controller *brdptr = motor_state[card];

	    if (brdptr == NULL)
		printf("    MM4000 controller %d connection failed.\n", card);
	    else
	    {
		struct MMcontroller *cntrl;

		cntrl = (struct MMcontroller *) brdptr->DevicePrivate;
		switch (cntrl->port_type)
		{
		case RS232_PORT: 
		    printf("    MM4000 controller %d port type = RS-232, id: %s \n", 
			   card, 
			   brdptr->ident);
		    break;
		case GPIB_PORT:
		    printf("    MM4000 controller %d port type = GPIB, id: %s \n", 
			   card, 
			   brdptr->ident);
		    break;
		default:
		    printf("    MM4000 controller %d port type = Unknown, id: %s \n", 
			   card, 
			   brdptr->ident);
		    break;
		}
	    }
	}
    }
    return (0);
}


static long init()
{
   /* 
    * We cannot call motor_init() here, because that function can do GPIB I/O,
    * and hence requires that the drvGPIB have already been initialized.
    * That cannot be guaranteed, so we need to call motor_init from device
    * support
    */
    /* Check for setup */
    if (MM4000_num_cards <= 0)
    {
	Debug(1, "init(): MM4000 driver disabled. MM4000Setup() missing from startup script.\n");
    }
    return ((long) 0);
}


STATIC void query_done(int card, int axis, struct mess_node *nodeptr)
{
}


/*********************************************************
 * Read the status and position of all motors on a card
 * start_status(int card)
 *            if card == -1 then start all cards
 *********************************************************/
STATIC void start_status(int card)
{
    struct MMcontroller *cntrl;
    int itera, status;

    if (card >= 0)
    {
	cntrl = (struct MMcontroller *) motor_state[card]->DevicePrivate;
	send_mess(card, READ_STATUS, NULL);
	status = recv_mess(card, cntrl->status_string, 1);
	if (status <= 0)
	    cntrl->status = COMM_ERR;
	else
	{
	    cntrl->status = NORMAL;
	    send_mess(card, READ_POSITION, NULL);
	    recv_mess(card, cntrl->position_string, 1);
	}
    }
    else
    {
	/* 
	 * For efficiency we send messages to all cards, then read all
	 * responses.  This minimizes the latency due to processing on each card
	 */
	for (itera = 0; (itera < total_cards) && motor_state[itera]; itera++)
	    send_mess(itera, READ_STATUS, NULL);
	for (itera = 0; (itera < total_cards) && motor_state[itera]; itera++)
	{
	    cntrl = (struct MMcontroller *) motor_state[itera]->DevicePrivate;
	    status = recv_mess(itera, cntrl->status_string, 1);
	    if (status <= 0)
		cntrl->status = COMM_ERR;
	    else
		cntrl->status = NORMAL;
	}
	for (itera = 0; (itera < total_cards) && motor_state[itera]; itera++)
	    send_mess(itera, READ_POSITION, NULL);
	for (itera = 0; (itera < total_cards) && motor_state[itera]; itera++)
	{
	    cntrl = (struct MMcontroller *) motor_state[itera]->DevicePrivate;
	    recv_mess(itera, cntrl->position_string, 1);
	}
    }
}


/**************************************************************
 * Parse status and position strings for a card and signal
 * set_status()
 ************************************************************/

STATIC int set_status(int card, int signal)
{
    struct MMcontroller *cntrl;
    struct mess_node *nodeptr;
    register struct mess_info *motor_info;
    /* Message parsing variables */
    char *p, *tok_save;
    char buff[BUFF_SIZE];
    int i, pos, status;
    int rtn_state;
    double motorData;

    cntrl = (struct MMcontroller *) motor_state[card]->DevicePrivate;
    motor_info = &(motor_state[card]->motor_info[signal]);
    nodeptr = motor_info->motor_motion;

    /* 
     * Parse the status string
     * Status string format: 1MSx,2MSy,3MSz,... where x, y and z are the status
     * bytes for the motors
     */
    pos = signal*5 + 3;	 /* Offset in status string */
    status = cntrl->status_string[pos];
    Debug(5, "set_status(): status byte = %x\n", status);

    if (status & M_MOTOR_DIRECTION)
	motor_info->status |= RA_DIRECTION;
    else
	motor_info->status &= ~RA_DIRECTION;

    if (status & M_AXIS_MOVING)
	motor_info->status &= ~RA_DONE;
    else
    {
	motor_info->status |= RA_DONE;
/* TEMPORARY FIX, Mark Rivers, 2/1/99. The MM4000 has reported that the
 * motor is done moving, which means that the "jerk time" is done.  However,
 * the axis can still be settling.  For now we put in a delay and poll the
 * motor position again. This is not a good long-term solution.
 */
	if (motor_info->pid_present == YES && drvMM4000ReadbackDelay != 0.)
	{
	    taskDelay((int)(drvMM4000ReadbackDelay * sysClkRateGet()));
	    send_mess(card, READ_POSITION, NULL);
	    recv_mess(card, cntrl->position_string, 1);
	}
    }

    if ((status & M_PLUS_LIMIT) || (status & M_MINUS_LIMIT))
	motor_info->status |= RA_OVERTRAVEL;
    else
	motor_info->status &= ~RA_OVERTRAVEL;

    if (status & M_HOME_SIGNAL)
	motor_info->status |= RA_HOME;
    else
	motor_info->status &= ~RA_HOME;

    if (status & M_MOTOR_POWER)
	motor_info->status &= ~EA_POSITION;
    else
	motor_info->status |= EA_POSITION;

    /* encoder status */
    motor_info->status &= ~EA_SLIP;
    motor_info->status &= ~EA_SLIP_STALL;
    motor_info->status &= ~EA_HOME;

    /* 
     * Parse motor position
     * Position string format: 1TP5.012,2TP1.123,3TP-100.567,...
     * Skip to substring for this motor, convert to double
     */

    strcpy(buff, cntrl->position_string);
    tok_save = NULL;
    p = strtok_r(buff, ",", &tok_save);
    for (i=0; i<signal; i++) p = strtok_r(NULL, ",", &tok_save);
    Debug(6, "set_status(): position substring = %s\n", p);
    motorData = atof(p+3) / cntrl->drive_resolution[signal];

    if (motorData == motor_info->position)
	motor_info->no_motion_count++;
    else
    {
	motor_info->position = NINT(motorData);
	if (motor_state[card]->motor_info[signal].encoder_present == YES)
	    motor_info->encoder_position = (int32_t) motorData;
	else
	    motor_info->encoder_position = 0;

	motor_info->no_motion_count = 0;
    }

    motor_info->status &= ~RA_PROBLEM;

    /* Parse motor velocity? */
    /* NEEDS WORK */

    motor_info->velocity = 0;

    if (!(motor_info->status & RA_DIRECTION))
	motor_info->velocity *= -1;

    rtn_state = (!motor_info->no_motion_count ||
		 (motor_info->status & (RA_OVERTRAVEL | RA_DONE | RA_PROBLEM))) ? 1 : 0;

    /* Test for post-move string. */
    if ((motor_info->status & RA_DONE || motor_info->status & RA_OVERTRAVEL) &&
	 nodeptr != 0 && nodeptr->postmsgptr != 0)
    {
	strcpy(buff, nodeptr->postmsgptr);
	strcat(buff, "\r");
	send_mess(card, buff, NULL);
	nodeptr->postmsgptr = NULL;
    }

    if (cntrl->status == COMM_ERR)
	motor_info->status |= CNTRL_COMM_ERR;
    else
	motor_info->status &= ~CNTRL_COMM_ERR;

    return(rtn_state);
}


/*****************************************************/
/* send a message to the MM4000 board		     */
/* send_mess()			                     */
/*****************************************************/
STATIC int send_mess(int card, char const *com, char inchar)
{
    struct MMcontroller *cntrl;

    if (strlen(com) > MAX_MSG_SIZE)
    {
	logMsg((char *) "drvMM4000.c:send_mess(); message size violation.\n",
	       0, 0, 0, 0, 0, 0);
	return (-1);
    }
    
    if (!motor_state[card])
    {
	logMsg((char *) "drvMM4000.c:send_mess() - invalid card #%d\n", card,
	       0, 0, 0, 0, 0);
	return (-1);
    }

    if (inchar != (char) NULL)
    {
	logMsg((char *) "drvMM4000.c:send_mess() - invalid argument = %c\n", inchar,
	       0, 0, 0, 0, 0);
	return (-1);
    }

    Debug(2, "send_mess(): message = %s\n", com);

    cntrl = (struct MMcontroller *) motor_state[card]->DevicePrivate;

    switch (cntrl->port_type)
    {
	case GPIB_PORT:
	    gpibIOSend(cntrl->gpibInfo, com, strlen(com), GPIB_TIMEOUT);
	    break;
	case RS232_PORT:
	    serialIOSend(cntrl->serialInfo, com, strlen(com), SERIAL_TIMEOUT);
	    break;
    }
    return (0);
}


/*****************************************************/
/* receive a message from the MM4000 board           */
/* recv_mess()			                     */
/*****************************************************/
STATIC int recv_mess(int card, char *com, int flag)
{
    struct MMcontroller *cntrl;
    int timeout;
    int len=0;

    /* Check that card exists */
    if (!motor_state[card])
	return (-1);

    cntrl = (struct MMcontroller *) motor_state[card]->DevicePrivate;

    switch (cntrl->port_type)
    {
	case GPIB_PORT:
	    if (flag == FLUSH)
		timeout = 0;
	    else
		timeout	= GPIB_TIMEOUT;
	    len = gpibIORecv(cntrl->gpibInfo, com, BUFF_SIZE, INPUT_TERMINATOR,
			     timeout);
	    break;
	case RS232_PORT:
	    if (flag == FLUSH)
		timeout = 0;
	    else
		timeout	= SERIAL_TIMEOUT;
	    len = serialIORecv(cntrl->serialInfo, com, BUFF_SIZE,
			       INPUT_TERMINATOR, timeout);
	    break;
    }

    if (len == 0)
	com[0] = '\0';
    else
	com[len-1] = '\0';

    Debug(2, "recv_mess(): message = \"%s\"\n", com);
    return (len);
}


/*****************************************************/
/* Setup system configuration                        */
/* MM4000Setup()                                     */
/*****************************************************/
int MM4000Setup(int num_cards,	/* maximum number of controllers in system.  */
	    int num_channels,	/* NOT Used. */
	    int scan_rate)	/* polling rate - 1/60 sec units.  */
{
    int itera;

    if (num_cards < 1 || num_cards > MM4000_NUM_CARDS)
	MM4000_num_cards = MM4000_NUM_CARDS;
    else
	MM4000_num_cards = num_cards;

    /* Set motor polling task rate */
    if (scan_rate >= 1 && scan_rate <= sysClkRateGet())
	motor_scan_rate = sysClkRateGet() / scan_rate;
    else
	motor_scan_rate = SCAN_RATE;

   /* 
    * Allocate space for motor_state structures.  Note this must be done
    * before MM4000Config is called, so it cannot be done in motor_init()
    * This means that we must allocate space for a card without knowing
    * if it really exists, which is not a serious problem
    */
    motor_state = (struct controller **) malloc(MM4000_num_cards *
						sizeof(struct controller *));

    for (itera = 0; itera < MM4000_num_cards; itera++)
	motor_state[itera] = (struct controller *) NULL;

    return (0);
}


/*****************************************************/
/* Configure a controller                            */
/* MM4000Config()                                    */
/*****************************************************/
int MM4000Config(int card,	/* card being configured */
            int port_type,      /* GPIB_PORT or RS232_PORT */
	    int addr1,          /* = link for GPIB or hideos_card for RS-232 */
            int addr2)          /* GPIB address or hideos_task */
{
    struct MMcontroller *cntrl;

    if (card < 0 || card >= MM4000_num_cards)
        return (ERROR);

    motor_state[card] = (struct controller *) malloc(sizeof(struct controller));
    motor_state[card]->DevicePrivate = malloc(sizeof(struct MMcontroller));
    cntrl = (struct MMcontroller *) motor_state[card]->DevicePrivate;

    switch (port_type)
    {
    case GPIB_PORT:
        cntrl->port_type = port_type;
        cntrl->gpib_link = addr1;
        cntrl->gpib_address = addr2;
        break;
    case RS232_PORT:
        cntrl->port_type = port_type;
        cntrl->serial_card = addr1;
        strcpy(cntrl->serial_task, (char *) addr2);
        break;
    default:
        return (ERROR);
    }
    return (0);
}



/*****************************************************/
/* initialize all software and hardware		     */
/* This is called from the initialization routine in */
/* device support.                                   */
/* motor_init()			                     */
/*****************************************************/
STATIC int motor_init()
{
    struct controller *brdptr;
    struct MMcontroller *cntrl;
    int card_index, motor_index, arg3, arg4;
    char axis_pos[BUFF_SIZE];
    char buff[BUFF_SIZE];
    char *tok_save, *pos_ptr;
    int total_axis = 0;
    int status, model_num, digits;
    BOOLEAN errind;

    initialized = ON;	/* Indicate that driver is initialized. */

    /* Check for setup */
    if (MM4000_num_cards <= 0)
	return (ERROR);

    for (card_index = 0; card_index < MM4000_num_cards; card_index++)
    {
	if (!motor_state[card_index])
	    continue;
	
	brdptr = motor_state[card_index];
	total_cards = card_index + 1;
	cntrl = (struct MMcontroller *) brdptr->DevicePrivate;

	/* Initialize communications channel */
	errind = OFF;
	switch (cntrl->port_type)
	{
	    case GPIB_PORT:
		cntrl->gpibInfo = gpibIOInit(cntrl->gpib_link,
					     cntrl->gpib_address);
		if (cntrl->gpibInfo == NULL)
		    errind = ON;
		break;
	    case RS232_PORT:
		cntrl->serialInfo = serialIOInit(cntrl->serial_card,
						 cntrl->serial_task);
		if (cntrl->serialInfo == NULL)
		    errind = ON;
		break;
	}

	if (errind == OFF)
	{
	    int retry = 0;

	    /* Send a message to the board, see if it exists */
	    /* flush any junk at input port - should not be any data available */
	    do
		recv_mess(card_index, buff, FLUSH);
	    while (strlen(buff) != 0);
    
	    do
	    {
		send_mess(card_index, READ_POSITION, NULL);
		status = recv_mess(card_index, axis_pos, 1);
		retry++;
		/* Return value is length of response string */
	    } while(status == 0 && retry < 3);
	}

	if (errind == OFF && status > 0)
	{
	    brdptr->localaddr = (char *) NULL;
	    brdptr->motor_in_motion = 0;
	    send_mess(card_index, STOP_ALL, NULL);	/* Stop all motors */
	    send_mess(card_index, GET_IDENT, NULL);	/* Read controller ID string */
	    recv_mess(card_index, buff, 1);
	    strcpy(brdptr->ident, &buff[2]);  /* Skip "VE" */

	    /* Set Motion Master model indicator. */
	    pos_ptr = strstr(brdptr->ident, "MM");
	    model_num = atoi(pos_ptr + 2);
	    if (model_num == 4000)
		cntrl->model = MM4000;
	    else if (model_num == 4005)
		cntrl->model = MM4005;
	    else
	    {
		logMsg((char *) "drvMM4000.c:motor_init() - invalid model = %s\n", (int) brdptr->ident,
		       0, 0, 0, 0, 0);
		return (ERROR);
	    }

	    send_mess(card_index, READ_POSITION, NULL);
	    recv_mess(card_index, axis_pos, 1);

	    /* The return string will tell us how many axes this controller has */
	    for (total_axis = 0, tok_save = NULL, pos_ptr = strtok_r(axis_pos, ",", &tok_save);
		    pos_ptr != 0; pos_ptr = strtok_r(NULL, ",", &tok_save), total_axis++)
		brdptr->motor_info[total_axis].motor_motion = NULL;

	    brdptr->total_axis = total_axis;

	    start_status(card_index);
	    for (motor_index = 0; motor_index < total_axis; motor_index++)
	    {
		struct mess_info *motor_info = &brdptr->motor_info[motor_index];
		int loop_state;

		motor_info->status = 0;
		motor_info->no_motion_count = 0;
		motor_info->encoder_position = 0;
		motor_info->position = 0;

                /* Determine if encoder present based on open/closed loop mode. */
		sprintf(buff, "%dTC\r", motor_index + 1);
	        send_mess(card_index, buff, NULL);
	        recv_mess(card_index, buff, 1);
		loop_state = atoi(&buff[3]);	/* Skip first 3 characters */
		if (loop_state != 0)
		{
                    motor_info->encoder_present = YES;
		    motor_info->status |= EA_PRESENT;
		    motor_info->pid_present = YES;
		    motor_info->status |= GAIN_SUPPORT;
		}

                /* Determine drive resolution. */
                sprintf(buff, "%dTU\r", motor_index + 1);
	        send_mess(card_index, buff, NULL);
	        recv_mess(card_index, buff, 1);
		cntrl->drive_resolution[motor_index] = atof(&buff[3]);

		digits = -log10(cntrl->drive_resolution[motor_index]) + 2;
		if (digits < 1)
		    digits = 1;
		cntrl->res_decpts[motor_index] = digits;              

		/* Save home preset position. */
                sprintf(buff, "%dXH\r", motor_index + 1);
	        send_mess(card_index, buff, NULL);
	        recv_mess(card_index, buff, 1);
		cntrl->home_preset[motor_index] = atof(&buff[3]);

                /* Determine low limit */
                sprintf(buff, "%dTL\r", motor_index + 1);
	        send_mess(card_index, buff, NULL);
	        recv_mess(card_index, buff, 1);
                motor_info->low_limit = atof(&buff[3]);

                /* Determine high limit */
                sprintf(buff, "%dTR\r", motor_index + 1);
	        send_mess(card_index, buff, NULL);
	        recv_mess(card_index, buff, 1);
                motor_info->high_limit = atof(&buff[3]);

		set_status(card_index, motor_index);  /* Read status of each motor */
	    }
	}
	else
	    motor_state[card_index] = (struct controller *) NULL;
    }

    motor_sem = semBCreate(SEM_Q_PRIORITY, SEM_EMPTY);
    any_motor_in_motion = 0;

    FASTLOCKINIT(&queue_lock);
    FASTLOCK(&queue_lock);
    mess_queue.head = (struct mess_node *) NULL;
    mess_queue.tail = (struct mess_node *) NULL;
    FASTUNLOCK(&queue_lock);

    FASTLOCKINIT(&freelist_lock);
    FASTLOCK(&freelist_lock);
    free_list.head = (struct mess_node *) NULL;
    free_list.tail = (struct mess_node *) NULL;
    FASTUNLOCK(&freelist_lock);

    if (sizeof(int) >= sizeof(char *))
    {
	arg3 = (int) (&MM4000_access);
	arg4 = 0;
    }
    else
    {
	arg3 = (int) ((long) &MM4000_access >> 16);
	arg4 = (int) ((long) &MM4000_access & 0xFFFF);
    }
    taskSpawn((char *) "MM4000_motor", 64, VX_FP_TASK | VX_STDIO, 5000, motor_task,
	      motor_scan_rate, arg3, arg4, 0, 0, 0, 0, 0, 0, 0);
    return (0);
}

