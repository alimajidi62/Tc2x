################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Libraries/iLLD/TC29B/Tricore/Ebu/BFlashSpansion/IfxEbu_BFlashSpansion.c 

C_DEPS += \
./Libraries/iLLD/TC29B/Tricore/Ebu/BFlashSpansion/IfxEbu_BFlashSpansion.d 

OBJS += \
./Libraries/iLLD/TC29B/Tricore/Ebu/BFlashSpansion/IfxEbu_BFlashSpansion.o 


# Each subdirectory must supply rules for building sources it contributes
Libraries/iLLD/TC29B/Tricore/Ebu/BFlashSpansion/%.o: ../Libraries/iLLD/TC29B/Tricore/Ebu/BFlashSpansion/%.c Libraries/iLLD/TC29B/Tricore/Ebu/BFlashSpansion/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: AURIX GCC Compiler'
	tricore-elf-gcc -std=c99 "@C:/dev/Green/Aurix/test2/TriCore Debug (GCC)/AURIX_GCC_Compiler-Include_paths__-I_.opt" -Og -g3 -gdwarf-3 -Wall -c -fmessage-length=0 -fno-common -fstrict-volatile-bitfields -fdata-sections -ffunction-sections -mtc161 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-Libraries-2f-iLLD-2f-TC29B-2f-Tricore-2f-Ebu-2f-BFlashSpansion

clean-Libraries-2f-iLLD-2f-TC29B-2f-Tricore-2f-Ebu-2f-BFlashSpansion:
	-$(RM) ./Libraries/iLLD/TC29B/Tricore/Ebu/BFlashSpansion/IfxEbu_BFlashSpansion.d ./Libraries/iLLD/TC29B/Tricore/Ebu/BFlashSpansion/IfxEbu_BFlashSpansion.o

.PHONY: clean-Libraries-2f-iLLD-2f-TC29B-2f-Tricore-2f-Ebu-2f-BFlashSpansion

