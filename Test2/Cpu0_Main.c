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
 * Combined demo  (TriBoard TC2X7 V1.0, active-low LEDs on PORT 33):
 *
 *   P33.6          : GTM/TOM0-CH2 hardware PWM - sawtooth brightness ramp
 *                    (0% -> 100% -> snap to 0, repeating, ~0.5 s cycle)
 *
 *   P33.7 - P33.13 : 7-bit binary counter via STM ISR, steps every 100 ms
 *
 * GTM clock:  FXCLK1 = 100 MHz / 16 = 6.25 MHz
 *   CM0 = 6250 ticks -> 1000 Hz PWM  (no visible flicker)
 *   Duty updated in STM ISR via SR1 shadow register
 *
 * STM ISR fires every STEP_TICKS = 1 000 000 ticks = 5 ms (at 200 MHz STM).
 *   Each call: advance PWM duty one step (~1%).
 *   Every 20 calls (= 100 ms): advance LED counter one step.
 */
#define GTM_PERIOD_TICKS   6250U
#define GTM_DUTY_STEP        63U   /* ~1% per 5 ms step */

#define STEP_TICKS      1000000UL  /* 5 ms at 200 MHz STM */
#define LED_DIVISOR          20U   /* 20 x 5 ms = 100 ms per counter step */

#define WDT_RELOAD         0x8000U /* ~5.4 s WDT timeout */
#define STM0_ISR_PRIO          10

/* -------------------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------------- */
IfxCpu_syncEvent            cpuSyncEvent = 0;
static IfxStm_CompareConfig stmConfig;

/* Visible in debugger Watch window */
volatile uint32 blinkCount = 0;

/* -------------------------------------------------------------------------
 * STM0 ISR  (fires every STEP_TICKS = 5 ms)
 *   - Updates GTM PWM duty via SR1 shadow register (sawtooth)
 *   - Updates binary counter on P33.7-P33.13 every 100 ms
 * ---------------------------------------------------------------------- */
IFX_INTERRUPT(stm0Isr, 0, STM0_ISR_PRIO)
{
    static uint16 duty     = 0;
    static uint8  ledTimer = 0;
    uint8 pin;

    IfxStm_clearCompareFlag(&MODULE_STM0, stmConfig.comparator);
    IfxStm_updateCompare(&MODULE_STM0, stmConfig.comparator,
                         IfxStm_getLower(&MODULE_STM0) + STEP_TICKS);

    /* --- PWM duty ramp: sawtooth 0 -> 100% -> snap to 0 --- */
    duty = (uint16)(duty + GTM_DUTY_STEP);
    if (duty >= GTM_PERIOD_TICKS)
        duty = 0;
    MODULE_GTM.TOM[0].CH2.SR1.U = duty;  /* shadow; loaded into CM1 at next period */

    /* --- Binary counter on P33.7-P33.13 every 100 ms --- */
    if (++ledTimer >= LED_DIVISOR)
    {
        ledTimer = 0;
        for (pin = 0; pin < 7; pin++)
        {
            if (blinkCount & (1u << pin))
                IfxPort_setPinLow(&MODULE_P33,  (uint8)(pin + 7));  /* bit=1 -> ON  */
            else
                IfxPort_setPinHigh(&MODULE_P33, (uint8)(pin + 7));  /* bit=0 -> OFF */
        }
        blinkCount = (blinkCount + 1u) & 0x7Fu;  /* 7-bit: 0-127 */
    }
}

/* -------------------------------------------------------------------------
 * GTM/TOM0-CH2 init  (P33.6, 1 kHz PWM, starts at 0% duty)
 * ---------------------------------------------------------------------- */
static void initGtmPwm(void)
{
    Ifx_GTM              *gtm = &MODULE_GTM;
    IfxGtm_Tom_Pwm_Config tomConfig;
    static IfxGtm_Tom_Pwm_Driver tomDriver;

    IfxGtm_enable(gtm);
    while (gtm->CLC.B.DISS != 0U) {}

    IfxGtm_Cmu_setGclkFrequency(gtm, IfxGtm_Cmu_getModuleFrequency(gtm));
    IfxGtm_Cmu_enableClocks(gtm, IFXGTM_CMU_CLKEN_FXCLK);

    IfxGtm_Tom_Pwm_initConfig(&tomConfig, gtm);
    tomConfig.tom                      = IfxGtm_Tom_0;
    tomConfig.tomChannel               = IfxGtm_Tom_Ch_2;
    tomConfig.clock                    = IfxGtm_Tom_Ch_ClkSrc_cmuFxclk1;
    tomConfig.period                   = GTM_PERIOD_TICKS;
    tomConfig.dutyCycle                = 0;
    tomConfig.signalLevel              = Ifx_ActiveState_low;
    tomConfig.synchronousUpdateEnabled = TRUE;   /* use SR1 for runtime updates */
    tomConfig.pin.outputPin            = &IfxGtm_TOM0_2_TOUT28_P33_6_OUT;
    tomConfig.pin.outputMode           = IfxPort_OutputMode_pushPull;
    tomConfig.pin.padDriver            = IfxPort_PadDriver_cmosAutomotiveSpeed1;
    tomConfig.immediateStartEnabled    = TRUE;

    IfxGtm_Tom_Pwm_init(&tomDriver, &tomConfig);
}

/* -------------------------------------------------------------------------
 * STM0 interrupt init
 * ---------------------------------------------------------------------- */
static void initStmInterrupt(void)
{
    IfxStm_initCompareConfig(&stmConfig);
    stmConfig.ticks           = STEP_TICKS;
    stmConfig.triggerPriority = STM0_ISR_PRIO;
    stmConfig.typeOfService   = IfxSrc_Tos_cpu0;
    IfxStm_initCompare(&MODULE_STM0, &stmConfig);
}

/* -------------------------------------------------------------------------
 * Core 0 entry point
 * ---------------------------------------------------------------------- */
void core0_main(void)
{
    uint8  pin;
    uint16 wdtPassword;

    IfxCpu_enableInterrupts();

    wdtPassword = IfxScuWdt_getCpuWatchdogPassword();
    IfxScuWdt_changeCpuWatchdogReload(wdtPassword, WDT_RELOAD);
    IfxScuWdt_disableSafetyWatchdog(IfxScuWdt_getSafetyWatchdogPassword());

    /* Configure all 8 LED pins as GPIO push-pull, all OFF */
    for (pin = 6; pin <= 13; pin++)
    {
        IfxPort_setPinMode(&MODULE_P33, pin, IfxPort_Mode_outputPushPullGeneral);
        IfxPort_setPinHigh(&MODULE_P33, pin);
    }

    initGtmPwm();       /* P33.6 -> GTM PWM (takes over pin from GPIO) */
    initStmInterrupt(); /* starts 5 ms ISR: PWM duty ramp + LED counter */

    IfxCpu_emitEvent(&cpuSyncEvent);
    IfxCpu_waitEvent(&cpuSyncEvent, 1);

    while(1)
    {
        IfxScuWdt_serviceCpuWatchdog(wdtPassword);
    }
}