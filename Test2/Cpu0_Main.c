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
#include "Dts/Dts/IfxDts_Dts.h"
#include "SharedMem.h"

/*
 * Combined demo  (TriBoard TC2X7 V1.0, active-low LEDs on PORT 33):
 *
 *   P33.6  - P33.10 : GTM/TOM0-CH2,3,4,1,0 hardware PWM - phased sawtooth wave
 *                     Five LEDs carry the same brightness ramp staggered by 1/5
 *                     period each, creating a wave that sweeps left to right.
 *
 *   P33.11 - P33.13 : 3-bit binary counter via STM ISR, steps every 100 ms
 *
 * GTM clock:  FXCLK1 = 100 MHz / 16 = 6.25 MHz
 *   CM0 = 6250 ticks -> 1000 Hz PWM  (no visible flicker)
 *   Each channel's SR1 is updated in the STM ISR; loaded into CM1 at next period.
 *
 * STM ISR fires every STEP_TICKS = 1 000 000 ticks = 5 ms (at 200 MHz STM).
 *   Each call: advance all 5 PWM duties one step (~1%).
 *   Every 20 calls (= 100 ms): advance the 3-bit LED counter one step.
 */
#define GTM_PERIOD_TICKS      6250U
#define GTM_DUTY_STEP_DEFAULT   63U   /* ~1% per 5 ms at room temperature */
#define PWM_CH_COUNT             5U   /* TOM0 CH0-CH4 on P33.10,9,6,7,8 */

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
volatile uint32 blinkCount  = 0;
volatile uint16 g_tempRaw   = 0;   /* raw 10-bit DTS RESULT field (0-1023)    */
volatile sint16 g_tempDegC  = 0;   /* die temperature in integer °C           */
volatile uint16 g_dutyStep  = GTM_DUTY_STEP_DEFAULT; /* maps temp → wave speed */

/* TC29x DSPR is non-cached scratchpad: safe for multi-core access via global bus */
Mailbox g_mbCpus;
SpinBox g_spinBox;

/* Results pulled from the mailboxes each round -- watch these in debugger */
volatile uint32 g_squareResult = 0;   /* CPU1 answer: input*input */
volatile uint32 g_sumResult    = 0;   /* CPU2 answer: inputA+inputB */
volatile uint32 g_mbCounter    = 0;   /* how many rounds completed */

/* -------------------------------------------------------------------------
 * STM0 ISR  (fires every STEP_TICKS = 5 ms)
 *   - Updates all 5 GTM PWM duties via SR1 shadow registers (phased sawtooth)
 *   - Updates 3-bit binary counter on P33.11-P33.13 every 100 ms
 * ---------------------------------------------------------------------- */
IFX_INTERRUPT(stm0Isr, 0, STM0_ISR_PRIO)
{
    /* Initial duties create a left-to-right wave: P33.6->P33.7->P33.8->P33.9->P33.10
     * Array is indexed by TOM channel number (CH0=P33.10 ... CH4=P33.8) */
    static uint16 duty[PWM_CH_COUNT] = {5000U, 3750U, 0U, 1250U, 2500U};
    static uint8  ledTimer = 0;
    uint8 pin, i;

    IfxStm_clearCompareFlag(&MODULE_STM0, stmConfig.comparator);
    IfxStm_updateCompare(&MODULE_STM0, stmConfig.comparator,
                         IfxStm_getLower(&MODULE_STM0) + STEP_TICKS);

    /* --- Sawtooth ramp on all 5 PWM channels, 1/5-period phase stagger --- */
    for (i = 0u; i < PWM_CH_COUNT; i++)
    {
        duty[i] = (uint16)(duty[i] + g_dutyStep);
        if (duty[i] >= GTM_PERIOD_TICKS)
            duty[i] = 0u;
    }
    MODULE_GTM.TOM[0].CH0.SR1.U = duty[0];  /* P33.10  phase 4/5 */
    MODULE_GTM.TOM[0].CH1.SR1.U = duty[1];  /* P33.9   phase 3/5 */
    MODULE_GTM.TOM[0].CH2.SR1.U = duty[2];  /* P33.6   phase 0   */
    MODULE_GTM.TOM[0].CH3.SR1.U = duty[3];  /* P33.7   phase 1/5 */
    MODULE_GTM.TOM[0].CH4.SR1.U = duty[4];  /* P33.8   phase 2/5 */

    /* --- 3-bit binary counter on P33.11-P33.13 every 100 ms --- */
    if (++ledTimer >= LED_DIVISOR)
    {
        ledTimer = 0;
        for (pin = 0u; pin < 3u; pin++)
        {
            if (blinkCount & (1u << pin))
                IfxPort_setPinLow(&MODULE_P33,  (uint8)(pin + 11u));
            else
                IfxPort_setPinHigh(&MODULE_P33, (uint8)(pin + 11u));
        }
        blinkCount = (blinkCount + 1u) & 0x07u;  /* 3-bit: 0-7 */
    }
}

/* -------------------------------------------------------------------------
 * GTM/TOM0-CH0..CH4 init  (P33.6-P33.10, 1 kHz PWM, phase-staggered)
 * ---------------------------------------------------------------------- */
static void initGtmPwm(void)
{
    Ifx_GTM              *gtm = &MODULE_GTM;
    IfxGtm_Tom_Pwm_Config tomConfig;
    static IfxGtm_Tom_Pwm_Driver tomDriver[PWM_CH_COUNT];

    /* Channel/pin tables ordered by physical LED position P33.6..P33.10 */
    static const IfxGtm_Tom_Ch pwmCh[PWM_CH_COUNT] = {
        IfxGtm_Tom_Ch_2, IfxGtm_Tom_Ch_3, IfxGtm_Tom_Ch_4,
        IfxGtm_Tom_Ch_1, IfxGtm_Tom_Ch_0
    };
    static const IfxGtm_Tom_ToutMap *pwmPin[PWM_CH_COUNT] = {
        &IfxGtm_TOM0_2_TOUT28_P33_6_OUT,
        &IfxGtm_TOM0_3_TOUT29_P33_7_OUT,
        &IfxGtm_TOM0_4_TOUT30_P33_8_OUT,
        &IfxGtm_TOM0_1_TOUT31_P33_9_OUT,
        &IfxGtm_TOM0_0_TOUT32_P33_10_OUT,
    };
    uint8 i;

    IfxGtm_enable(gtm);
    while (gtm->CLC.B.DISS != 0U) {}

    IfxGtm_Cmu_setGclkFrequency(gtm, IfxGtm_Cmu_getModuleFrequency(gtm));
    IfxGtm_Cmu_enableClocks(gtm, IFXGTM_CMU_CLKEN_FXCLK);

    IfxGtm_Tom_Pwm_initConfig(&tomConfig, gtm);
    tomConfig.tom                      = IfxGtm_Tom_0;
    tomConfig.clock                    = IfxGtm_Tom_Ch_ClkSrc_cmuFxclk1;
    tomConfig.period                   = GTM_PERIOD_TICKS;
    tomConfig.dutyCycle                = 0;
    tomConfig.signalLevel              = Ifx_ActiveState_low;
    tomConfig.synchronousUpdateEnabled = TRUE;
    tomConfig.pin.outputMode           = IfxPort_OutputMode_pushPull;
    tomConfig.pin.padDriver            = IfxPort_PadDriver_cmosAutomotiveSpeed1;
    tomConfig.immediateStartEnabled    = TRUE;

    for (i = 0u; i < PWM_CH_COUNT; i++)
    {
        tomConfig.tomChannel    = pwmCh[i];
        tomConfig.pin.outputPin = pwmPin[i];
        IfxGtm_Tom_Pwm_init(&tomDriver[i], &tomConfig);
    }
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
 * DTS init  — discard first two results per iLLD note (sensor warm-up)
 * ---------------------------------------------------------------------- */
static void initDts(void)
{
    IfxDts_Dts_Config dtsConfig;
    IfxDts_Dts_initModuleConfig(&dtsConfig);
    dtsConfig.lowerTemperatureLimit = -40.0f;
    dtsConfig.upperTemperatureLimit =  150.0f;
    IfxDts_Dts_initModule(&dtsConfig);

    IfxDts_Dts_startSensor();
    while (IfxDts_Dts_isBusy()) {}
    IfxDts_Dts_startSensor();
    while (IfxDts_Dts_isBusy()) {}
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

    /* Configure counter LED pins as GPIO push-pull, all OFF (P33.6-10 owned by GTM) */
    for (pin = 11; pin <= 13; pin++)
    {
        IfxPort_setPinMode(&MODULE_P33, pin, IfxPort_Mode_outputPushPullGeneral);
        IfxPort_setPinHigh(&MODULE_P33, pin);
    }

    initGtmPwm();       /* P33.6 -> GTM PWM (takes over pin from GPIO) */
    initStmInterrupt(); /* starts 5 ms ISR: PWM duty ramp + LED counter */
    initDts();          /* warm up die temperature sensor               */

    IfxCpu_emitEvent(&cpuSyncEvent);
    IfxCpu_waitEvent(&cpuSyncEvent, 1);

    /* Explicitly initialise NOLOAD shared memory (no startup copy from flash) */
    uint32 w;
    for (w = 0u; w < MB_NWORKERS; w++)
    {
        g_mbCpus.cmd[w] = MB_IDLE;
        g_mbCpus.inputA[w] = 0u;
        g_mbCpus.inputB[w] = 0u;
        g_mbCpus.result[w] = 0u;
    }

    g_spinBox.lock        = 0u;
    g_spinBox.total       = 0u;
    g_spinBox.perCore[0u] = 0u;
    g_spinBox.perCore[1u] = 0u;
    g_spinBox.perCore[2u] = 0u;

    /* Every 500 ms: post work to CPU1 (square) and CPU2 (add), collect results.
     * Watch in debugger: g_mbCounter, g_squareResult, g_sumResult. */
    uint64 nextRoundTick = IfxStm_get(&MODULE_STM0) + 100000000ULL; /* 500 ms */

    while(1)
    {
        IfxScuWdt_serviceCpuWatchdog(wdtPassword);

        if (IfxStm_get(&MODULE_STM0) < nextRoundTick)
            continue;
        nextRoundTick += 100000000ULL;

        /* --- DTS temperature reading ----------------------------------------- */
        IfxDts_Dts_startSensor();
        while (IfxDts_Dts_isBusy()) {}
        g_tempRaw  = IfxDts_Dts_getTemperatureValue();
        g_tempDegC = (sint16)(((sint32)g_tempRaw * 467L - 285500L) / 1000L);
        /* map °C linearly to wave speed: 0°C→16 steps, 100°C→250 steps */
        {
            sint32 step = 16L + ((sint32)g_tempDegC * 234L) / 100L;
            if (step <   16) step =   16;
            if (step > 1000) step = 1000;
            g_dutyStep = (uint16)step;
        }

        /* --- Post work to CPU1 (square) -------------------------------- */
        g_mbCpus.inputA[MB_CPU1] = g_mbCounter;
        g_mbCpus.inputB[MB_CPU1] = 0u;
        g_mbCpus.cmd[MB_CPU1]    = MB_REQ;

        /* --- Post work to CPU2 (add) ----------------------------------- */
        g_mbCpus.inputA[MB_CPU2] = g_mbCounter;
        g_mbCpus.inputB[MB_CPU2] = g_mbCounter + 1u;
        g_mbCpus.cmd[MB_CPU2]    = MB_REQ;

        /* --- Wait for CPU1 result -------------------------------------- */
        while (g_mbCpus.cmd[MB_CPU1] != MB_DONE) {}
        g_squareResult          = g_mbCpus.result[MB_CPU1];
        g_mbCpus.cmd[MB_CPU1]   = MB_IDLE;

        /* --- Wait for CPU2 result -------------------------------------- */
        while (g_mbCpus.cmd[MB_CPU2] != MB_DONE) {}
        g_sumResult             = g_mbCpus.result[MB_CPU2];
        g_mbCpus.cmd[MB_CPU2]   = MB_IDLE;

        SpinBox_add(&g_spinBox, 0u, 1u);
        g_mbCounter++;
    }
}