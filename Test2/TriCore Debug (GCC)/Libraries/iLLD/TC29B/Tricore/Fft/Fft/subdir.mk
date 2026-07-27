################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Libraries/iLLD/TC29B/Tricore/Fft/Fft/IfxFft_Fft.c 

C_DEPS += \
./Libraries/iLLD/TC29B/Tricore/Fft/Fft/IfxFft_Fft.d 

OBJS += \
./Libraries/iLLD/TC29B/Tricore/Fft/Fft/IfxFft_Fft.o 


# Each subdirectory must supply rules for building sources it contributes
Libraries/iLLD/TC29B/Tricore/Fft/Fft/%.o: ../Libraries/iLLD/TC29B/Tricore/Fft/Fft/%.c Libraries/iLLD/TC29B/Tricore/Fft/Fft/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: AURIX GCC Compiler'
	tricore-elf-gcc -std=c99 "@C:/dev/Green/Aurix/test2/TriCore Debug (GCC)/AURIX_GCC_Compiler-Include_paths__-I_.opt" -Og -g3 -gdwarf-3 -Wall -c -fmessage-length=0 -fno-common -fstrict-volatile-bitfields -fdata-sections -ffunction-sections -mtc161 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-Libraries-2f-iLLD-2f-TC29B-2f-Tricore-2f-Fft-2f-Fft

clean-Libraries-2f-iLLD-2f-TC29B-2f-Tricore-2f-Fft-2f-Fft:
	-$(RM) ./Libraries/iLLD/TC29B/Tricore/Fft/Fft/IfxFft_Fft.d ./Libraries/iLLD/TC29B/Tricore/Fft/Fft/IfxFft_Fft.o

.PHONY: clean-Libraries-2f-iLLD-2f-TC29B-2f-Tricore-2f-Fft-2f-Fft

