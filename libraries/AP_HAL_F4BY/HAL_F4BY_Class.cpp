/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#include <AP_HAL.h>

#if CONFIG_HAL_BOARD == HAL_BOARD_F4BY

#include <AP_HAL_F4BY.h>
#include "AP_HAL_F4BY_Namespace.h"
#include "HAL_F4BY_Class.h"
#include "Scheduler.h"
#include "UARTDriver.h"
#include "Storage.h"
#include "RCInput.h"
#include "RCOutput.h"
#include "AnalogIn.h"
#include "Util.h"
#include "GPIO.h"

#include <AP_HAL_Empty.h>
#include <AP_HAL_Empty_Private.h>

#include <stdlib.h>
#include <systemlib/systemlib.h>
#include <nuttx/config.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <poll.h>
#include <drivers/drv_hrt.h>

using namespace F4BY;

static Empty::EmptySemaphore  i2cSemaphore;
static Empty::EmptyI2CDriver  i2cDriver(&i2cSemaphore);
static Empty::EmptySPIDeviceManager spiDeviceManager;
//static Empty::EmptyGPIO gpioDriver;

static F4BYScheduler schedulerInstance;
static F4BYStorage storageDriver;
static F4BYRCInput rcinDriver;
static F4BYRCOutput rcoutDriver;
static F4BYAnalogIn analogIn;
static F4BYUtil utilInstance;
static F4BYGPIO gpioDriver;

#define UARTA_DEFAULT_DEVICE "/dev/ttyACM0"
#define UARTB_DEFAULT_DEVICE "/dev/ttyS2"
#define UARTC_DEFAULT_DEVICE "/dev/ttyS1"
#define UARTD_DEFAULT_DEVICE "/dev/null"
#define UARTE_DEFAULT_DEVICE "/dev/null"

// 3 UART drivers, for GPS plus two mavlink-enabled devices
static F4BYUARTDriver uartADriver(UARTA_DEFAULT_DEVICE, "APM_uartA");
static F4BYUARTDriver uartBDriver(UARTB_DEFAULT_DEVICE, "APM_uartB");
static F4BYUARTDriver uartCDriver(UARTC_DEFAULT_DEVICE, "APM_uartC");
static F4BYUARTDriver uartDDriver(UARTD_DEFAULT_DEVICE, "APM_uartD");
static F4BYUARTDriver uartEDriver(UARTE_DEFAULT_DEVICE, "APM_uartE");

HAL_F4BY::HAL_F4BY() :
    AP_HAL::HAL(
        &uartADriver,  /* uartA */
        &uartBDriver,  /* uartB */
        &uartCDriver,  /* uartC */
        &uartDDriver,  /* uartD */
        &uartEDriver,  /* uartE */
        &i2cDriver, /* i2c */
        &spiDeviceManager, /* spi */
        &analogIn, /* analogin */
        &storageDriver, /* storage */
        &uartADriver, /* console */
        &gpioDriver, /* gpio */
        &rcinDriver,  /* rcinput */
        &rcoutDriver, /* rcoutput */
        &schedulerInstance, /* scheduler */
        &utilInstance) /* util */
{}

bool _f4by_thread_should_exit = false;        /**< Daemon exit flag */
static bool thread_running = false;        /**< Daemon status flag */
static int daemon_task;                /**< Handle of daemon task / thread */
bool f4by_ran_overtime;

extern const AP_HAL::HAL& hal;

/*
  set the priority of the main APM task
 */
static void set_priority(uint8_t priority)
{
    struct sched_param param;
    param.sched_priority = priority;
    sched_setscheduler(daemon_task, SCHED_FIFO, &param);    
}

/*
  this is called when loop() takes more than 1 second to run. If that
  happens then something is blocking for a long time in the main
  sketch - probably waiting on a low priority driver. Set the priority
  of the APM task low to let the driver run.
 */
static void loop_overtime(void *)
{
    set_priority(APM_OVERTIME_PRIORITY);
    f4by_ran_overtime = true;
}

static int main_loop(int argc, char **argv)
{
    extern void setup(void);
    extern void loop(void);


    hal.uartA->begin(115200);
    hal.uartB->begin(38400);
    hal.uartC->begin(57600);
    hal.uartD->begin(57600);
    hal.scheduler->init(NULL);
    hal.rcin->init(NULL);
    hal.rcout->init(NULL);
    hal.analogin->init(NULL);
    hal.gpio->init();


    /*
      run setup() at low priority to ensure CLI doesn't hang the
      system, and to allow initial sensor read loops to run
     */
    set_priority(APM_STARTUP_PRIORITY);

    schedulerInstance.hal_initialized();

    setup();
    hal.scheduler->system_initialized();

    perf_counter_t perf_loop = perf_alloc(PC_ELAPSED, "APM_loop");
    perf_counter_t perf_overrun = perf_alloc(PC_COUNT, "APM_overrun");
    struct hrt_call loop_overtime_call;

    thread_running = true;

    /*
      switch to high priority for main loop
     */
    set_priority(APM_MAIN_PRIORITY);

    while (!_f4by_thread_should_exit) {
        perf_begin(perf_loop);
        
        /*
          this ensures a tight loop waiting on a lower priority driver
          will eventually give up some time for the driver to run. It
          will only ever be called if a loop() call runs for more than
          0.1 second
         */
        hrt_call_after(&loop_overtime_call, 100000, (hrt_callout)loop_overtime, NULL);

        loop();

        if (f4by_ran_overtime) {
            /*
              we ran over 1s in loop(), and our priority was lowered
              to let a driver run. Set it back to high priority now.
             */
            set_priority(APM_MAIN_PRIORITY);
            perf_count(perf_overrun);
            f4by_ran_overtime = false;
        }

        perf_end(perf_loop);

        /*
          give up 500 microseconds of time, to ensure drivers get a
          chance to run. This relies on the accurate semaphore wait
          using hrt in semaphore.cpp
         */
        hal.scheduler->delay_microseconds(500);
    }
    thread_running = false;
    return 0;
}

static void usage(void)
{
    printf("Usage: %s [options] {start,stop,status}\n", SKETCHNAME);
    printf("Options:\n");
    printf("\t-d  DEVICE         set terminal device (default %s)\n", UARTA_DEFAULT_DEVICE);
    printf("\t-d2 DEVICE         set second terminal device (default %s)\n", UARTC_DEFAULT_DEVICE);
    printf("\n");
}


void HAL_F4BY::init(int argc, char * const argv[]) const 
{
    int i;
    const char *deviceA = UARTA_DEFAULT_DEVICE;
    const char *deviceC = UARTC_DEFAULT_DEVICE;
    const char *deviceD = UARTD_DEFAULT_DEVICE;

    if (argc < 1) {
        printf("%s: missing command (try '%s start')", 
               SKETCHNAME, SKETCHNAME);
        usage();
        exit(1);
    }

    for (i=0; i<argc; i++) {
        if (strcmp(argv[i], "start") == 0) {
            if (thread_running) {
                printf("%s already running\n", SKETCHNAME);
                /* this is not an error */
                exit(0);
            }

            uartADriver.set_device_path(deviceA);
            uartCDriver.set_device_path(deviceC);
            uartDDriver.set_device_path(deviceD);
            printf("Starting %s uartA=%s uartC=%s uartD=%s\n", 
                   SKETCHNAME, deviceA, deviceC, deviceD);

            _f4by_thread_should_exit = false;
            daemon_task = task_spawn_cmd(SKETCHNAME,
                                     SCHED_FIFO,
                                     APM_MAIN_PRIORITY,
                                     8192,
                                     main_loop,
                                     NULL);
            exit(0);
        }

        if (strcmp(argv[i], "stop") == 0) {
            _f4by_thread_should_exit = true;
            exit(0);
        }
 
        if (strcmp(argv[i], "status") == 0) {
            if (_f4by_thread_should_exit && thread_running) {
                printf("\t%s is exiting\n", SKETCHNAME);
            } else if (thread_running) {
                printf("\t%s is running\n", SKETCHNAME);
            } else {
                printf("\t%s is not started\n", SKETCHNAME);
            }
            exit(0);
        }

        if (strcmp(argv[i], "-d") == 0) {
            // set terminal device
            if (argc > i + 1) {
                deviceA = strdup(argv[i+1]);
            } else {
                printf("missing parameter to -d DEVICE\n");
                usage();
                exit(1);
            }
        }

        if (strcmp(argv[i], "-d2") == 0) {
            // set uartC terminal device
            if (argc > i + 1) {
                deviceC = strdup(argv[i+1]);
            } else {
                printf("missing parameter to -d2 DEVICE\n");
                usage();
                exit(1);
            }
        }

        if (strcmp(argv[i], "-d3") == 0) {
            // set uartD terminal device
            if (argc > i + 1) {
                deviceD = strdup(argv[i+1]);
            } else {
                printf("missing parameter to -d3 DEVICE\n");
                usage();
                exit(1);
            }
        }
    }
 
    usage();
    exit(1);
}

const HAL_F4BY AP_HAL_F4BY;

#endif // CONFIG_HAL_BOARD == HAL_BOARD_F4BY

