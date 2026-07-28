/**********************************************************************************************************************
 * \file Cpu0_Main.c
 * \copyright Copyright (C) Infineon Technologies AG 2019
 *
 * Boost Software License - Version 1.0 - August 17th, 2003
 *********************************************************************************************************************/
#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"
#include "IfxPort.h"
#include "Stm/Std/IfxStm.h"
#include "Gtm/Tom/Pwm/IfxGtm_Tom_Pwm.h"

/*
 * GTM/TOM PWM blink test - P33.6 (active-low: LOW=ON, HIGH=OFF)
 * Freq : 0.5 Hz  ->  LED ON 1 s, LED OFF 1 s
 * Duty : 50 %
 *
 * Clock: FXCLK4 = GCLK/32768 = 200 MHz/32768 = ~6104 Hz
 *   CM0 (period) = 12208 ticks -> 2.0 s period
 *   CM1 (duty)   =  6104 ticks -> 1.0 s on-time  (50%)
 *
 * ROOT CAUSE FIX: synchronousUpdateEnabled = TRUE
 *   With FALSE (default), init() writes CM0/CM1 directly but then the
 *   FUPD trigger overwrites them from SR0/SR1 (which hold reset values 0).
 *   With TRUE, init() writes period/duty into SR0/SR1 first, the trigger
 *   then correctly loads CM0=period and CM1=duty.
 *
 * DIAGNOSTIC: 3 GPIO blinks before GTM starts prove the LED hardware works.
 *   - See 3 blinks  : LED and P33.6 circuit OK, issue is GTM only
 *   - No blinks     : Check P33.6 LED circuit / board variant
 *   - 3 blinks then continuous 0.5 Hz blink: GTM works perfectly
 *   - 3 blinks then LED stays OFF: GTM output stuck HIGH, clock issue
 */
#define GTM_PERIOD_TICKS   6250U    /* FXCLK1: 6.25 MHz / 6250 = 1000 Hz PWM  */
#define GTM_DUTY_TICKS        0U    /* start at 0% - breathing loop changes it  */
#define GTM_DUTY_STEP        63U    /* ~1% per step (63/6250)                   */
#define STEP_TICKS      1000000UL  /* 50 ms per step at 200 MHz STM            */
#define HALF_SECOND_TICKS  100000000UL   /* 100 M ticks / 200 MHz = 500 ms */

IfxCpu_syncEvent cpuSyncEvent = 0;

static void initGtmPwm(void)
{
    Ifx_GTM              *gtm = &MODULE_GTM;
    IfxGtm_Tom_Pwm_Config tomConfig;
    /* static: survives function return; avoids UB if start/stop called later */
    static IfxGtm_Tom_Pwm_Driver tomDriver;

    /* Enable GTM, then WAIT until module is actually active */
    IfxGtm_enable(gtm);
    while (gtm->CLC.B.DISS != 0U) {}

    /* Set GCLK = module clock (200 MHz), enable FXCLK group */
    IfxGtm_Cmu_setGclkFrequency(gtm, IfxGtm_Cmu_getModuleFrequency(gtm));
    IfxGtm_Cmu_enableClocks(gtm, IFXGTM_CMU_CLKEN_FXCLK);

    IfxGtm_Tom_Pwm_initConfig(&tomConfig, gtm);
    tomConfig.tom        = IfxGtm_Tom_0;
    tomConfig.tomChannel = IfxGtm_Tom_Ch_2;
    tomConfig.clock      = IfxGtm_Tom_Ch_ClkSrc_cmuFxclk1; /* 100 MHz / 16 = 6.25 MHz */
    tomConfig.period     = GTM_PERIOD_TICKS;
    tomConfig.dutyCycle  = GTM_DUTY_TICKS;
    tomConfig.signalLevel           = Ifx_ActiveState_low;
    /* KEY FIX: use shadow registers (SR0/SR1) as the load path for CM0/CM1.
     * Without this, the FUPD trigger overwrites CM0/CM1 from SR0/SR1 reset
     * values (= 0), leaving CM0=CM1=0 and the output stuck permanently HIGH. */
    tomConfig.synchronousUpdateEnabled = TRUE;
    tomConfig.pin.outputPin  = &IfxGtm_TOM0_2_TOUT28_P33_6_OUT;
    tomConfig.pin.outputMode = IfxPort_OutputMode_pushPull;
    tomConfig.pin.padDriver  = IfxPort_PadDriver_cmosAutomotiveSpeed1;
    tomConfig.immediateStartEnabled = TRUE;

    IfxGtm_Tom_Pwm_init(&tomDriver, &tomConfig);
}

void core0_main(void)
{
    uint8 i;

    IfxCpu_enableInterrupts();

    /* Disable both watchdogs completely (no servicing needed in while loop) */
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    IfxScuWdt_disableSafetyWatchdog(IfxScuWdt_getSafetyWatchdogPassword());

    /* Configure P33.6 as GPIO push-pull, start OFF (HIGH = LED off) */
    IfxPort_setPinMode(&MODULE_P33, 6, IfxPort_Mode_outputPushPullGeneral);
    IfxPort_setPinHigh(&MODULE_P33, 6);

    /* ---------------------------------------------------------------
     * GPIO blink test (3 x 500 ms ON / 500 ms OFF)
     * Confirms LED hardware works BEFORE GTM is involved.
     * If you see 3 blinks -> LED and circuit OK.
     * --------------------------------------------------------------- */
    for (i = 0; i < 3; i++)
    {
        IfxStm_waitTicks(&MODULE_STM0, HALF_SECOND_TICKS);
        IfxPort_setPinLow(&MODULE_P33, 6);    /* LED ON  */
        IfxStm_waitTicks(&MODULE_STM0, HALF_SECOND_TICKS);
        IfxPort_setPinHigh(&MODULE_P33, 6);   /* LED OFF */
    }

    /* After 3 blinks: pin is OFF in GPIO mode.
     * Turn P33.7 ON permanently as full-brightness reference (100%).
     * Then GTM takes P33.6 at 30% PWM so you can compare brightness. */
    IfxPort_setPinMode(&MODULE_P33, 7, IfxPort_Mode_outputPushPullGeneral);
    IfxPort_setPinLow(&MODULE_P33, 7);   /* P33.7 LED always ON (active-low) */

    initGtmPwm();

    IfxCpu_emitEvent(&cpuSyncEvent);
    IfxCpu_waitEvent(&cpuSyncEvent, 1);

    /* Breathing loop: ramp duty 0% -> 100% -> 0% -> ...
     * One step every 50 ms, ~100 steps per ramp = ~5 s per ramp.
     * Write to SR1 (shadow register); TOM loads it into CM1
     * automatically at each period boundary (every 1 ms). */
    {
        uint16  duty       = 0;
        boolean increasing = TRUE;

        while(1)
        {
            IfxStm_waitTicks(&MODULE_STM0, STEP_TICKS);

            if (increasing)
            {
                duty = (uint16)(duty + GTM_DUTY_STEP);
                if (duty >= GTM_PERIOD_TICKS)
                    { duty = 0; increasing = TRUE; }
            }
            else
            {
                if (duty <= GTM_DUTY_STEP)
                    { duty = 0; increasing = TRUE; }
                else
                    duty = (uint16)(duty - GTM_DUTY_STEP);
            }

            /* Update duty via shadow register - glitch-free, takes effect
             * on the next period boundary (within 1 ms at 1 kHz) */
            MODULE_GTM.TOM[0].CH2.SR1.U = duty;
        }
    }
}
