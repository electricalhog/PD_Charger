#[allow(unused)]
macro_rules! foreach_flash_region {
    ($($pat:tt => $code:tt;)*) => {
        macro_rules! __foreach_flash_region_inner {
            $(($pat) => $code;)*
            ($_:tt) => {}
        }
        __foreach_flash_region_inner!((Bank1Region,8,2048));
    };
}#[allow(unused)]
macro_rules! foreach_interrupt {
    ($($pat:tt => $code:tt;)*) => {
        macro_rules! __foreach_interrupt_inner {
            $(($pat) => $code;)*
            ($_:tt) => {}
        }
        __foreach_interrupt_inner!((ADC1,adc,ADC,GLOBAL,ADC1_2));
        __foreach_interrupt_inner!((ADC2,adc,ADC,GLOBAL,ADC1_2));
        __foreach_interrupt_inner!((CORDIC,cordic,CORDIC,GLOBAL,CORDIC));
        __foreach_interrupt_inner!((DAC1,dac,DAC,GLOBAL,TIM6_DAC));
        __foreach_interrupt_inner!((DAC3,dac,DAC,GLOBAL,TIM6_DAC));
        __foreach_interrupt_inner!((DMA1,bdma,DMA,CH1,DMA1_CHANNEL1));
        __foreach_interrupt_inner!((DMA1,bdma,DMA,CH2,DMA1_CHANNEL2));
        __foreach_interrupt_inner!((DMA1,bdma,DMA,CH3,DMA1_CHANNEL3));
        __foreach_interrupt_inner!((DMA1,bdma,DMA,CH4,DMA1_CHANNEL4));
        __foreach_interrupt_inner!((DMA1,bdma,DMA,CH5,DMA1_CHANNEL5));
        __foreach_interrupt_inner!((DMA1,bdma,DMA,CH6,DMA1_CHANNEL6));
        __foreach_interrupt_inner!((DMA2,bdma,DMA,CH1,DMA2_CHANNEL1));
        __foreach_interrupt_inner!((DMA2,bdma,DMA,CH2,DMA2_CHANNEL2));
        __foreach_interrupt_inner!((DMA2,bdma,DMA,CH3,DMA2_CHANNEL3));
        __foreach_interrupt_inner!((DMA2,bdma,DMA,CH4,DMA2_CHANNEL4));
        __foreach_interrupt_inner!((DMA2,bdma,DMA,CH5,DMA2_CHANNEL5));
        __foreach_interrupt_inner!((DMA2,bdma,DMA,CH6,DMA2_CHANNEL6));
        __foreach_interrupt_inner!((DMAMUX1,dmamux,DMAMUX,OVR,DMAMUX_OVR));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI0,EXTI0));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI1,EXTI1));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI10,EXTI15_10));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI11,EXTI15_10));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI12,EXTI15_10));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI13,EXTI15_10));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI14,EXTI15_10));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI15,EXTI15_10));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI2,EXTI2));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI3,EXTI3));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI4,EXTI4));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI5,EXTI9_5));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI6,EXTI9_5));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI7,EXTI9_5));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI8,EXTI9_5));
        __foreach_interrupt_inner!((EXTI,exti,EXTI,EXTI9,EXTI9_5));
        __foreach_interrupt_inner!((FDCAN1,can,FDCAN,IT0,FDCAN1_IT0));
        __foreach_interrupt_inner!((FDCAN1,can,FDCAN,IT1,FDCAN1_IT1));
        __foreach_interrupt_inner!((FLASH,flash,FLASH,GLOBAL,FLASH));
        __foreach_interrupt_inner!((FMAC,fmac,FMAC,GLOBAL,FMAC));
        __foreach_interrupt_inner!((I2C1,i2c,I2C,ER,I2C1_ER));
        __foreach_interrupt_inner!((I2C1,i2c,I2C,EV,I2C1_EV));
        __foreach_interrupt_inner!((I2C2,i2c,I2C,ER,I2C2_ER));
        __foreach_interrupt_inner!((I2C2,i2c,I2C,EV,I2C2_EV));
        __foreach_interrupt_inner!((I2C3,i2c,I2C,ER,I2C3_ER));
        __foreach_interrupt_inner!((I2C3,i2c,I2C,EV,I2C3_EV));
        __foreach_interrupt_inner!((LPUART1,usart,LPUART,GLOBAL,LPUART1));
        __foreach_interrupt_inner!((RCC,rcc,RCC,CRS,CRS));
        __foreach_interrupt_inner!((RCC,rcc,RCC,GLOBAL,RCC));
        __foreach_interrupt_inner!((RCC,rcc,RCC,LSECSS,RTC_TAMP_LSECSS));
        __foreach_interrupt_inner!((RNG,rng,RNG,GLOBAL,RNG));
        __foreach_interrupt_inner!((RTC,rtc,RTC,ALARM,RTC_ALARM));
        __foreach_interrupt_inner!((RTC,rtc,RTC,TAMP,RTC_TAMP_LSECSS));
        __foreach_interrupt_inner!((RTC,rtc,RTC,WKUP,RTC_WKUP));
        __foreach_interrupt_inner!((SAI1,sai,SAI,GLOBAL,SAI1));
        __foreach_interrupt_inner!((SPI1,spi,SPI,GLOBAL,SPI1));
        __foreach_interrupt_inner!((SPI2,spi,SPI,GLOBAL,SPI2));
        __foreach_interrupt_inner!((SPI3,spi,SPI,GLOBAL,SPI3));
        __foreach_interrupt_inner!((TIM1,timer,TIM_ADV,BRK,TIM1_BRK_TIM15));
        __foreach_interrupt_inner!((TIM1,timer,TIM_ADV,CC,TIM1_CC));
        __foreach_interrupt_inner!((TIM1,timer,TIM_ADV,COM,TIM1_TRG_COM_TIM17));
        __foreach_interrupt_inner!((TIM1,timer,TIM_ADV,TRG,TIM1_TRG_COM_TIM17));
        __foreach_interrupt_inner!((TIM1,timer,TIM_ADV,UP,TIM1_UP_TIM16));
        __foreach_interrupt_inner!((TIM15,timer,TIM_GP16,BRK,TIM1_BRK_TIM15));
        __foreach_interrupt_inner!((TIM15,timer,TIM_GP16,CC,TIM1_BRK_TIM15));
        __foreach_interrupt_inner!((TIM15,timer,TIM_GP16,COM,TIM1_BRK_TIM15));
        __foreach_interrupt_inner!((TIM15,timer,TIM_GP16,TRG,TIM1_BRK_TIM15));
        __foreach_interrupt_inner!((TIM15,timer,TIM_GP16,UP,TIM1_BRK_TIM15));
        __foreach_interrupt_inner!((TIM16,timer,TIM_GP16,BRK,TIM1_UP_TIM16));
        __foreach_interrupt_inner!((TIM16,timer,TIM_GP16,CC,TIM1_UP_TIM16));
        __foreach_interrupt_inner!((TIM16,timer,TIM_GP16,COM,TIM1_UP_TIM16));
        __foreach_interrupt_inner!((TIM16,timer,TIM_GP16,TRG,TIM1_UP_TIM16));
        __foreach_interrupt_inner!((TIM16,timer,TIM_GP16,UP,TIM1_UP_TIM16));
        __foreach_interrupt_inner!((TIM17,timer,TIM_GP16,BRK,TIM1_TRG_COM_TIM17));
        __foreach_interrupt_inner!((TIM17,timer,TIM_GP16,CC,TIM1_TRG_COM_TIM17));
        __foreach_interrupt_inner!((TIM17,timer,TIM_GP16,COM,TIM1_TRG_COM_TIM17));
        __foreach_interrupt_inner!((TIM17,timer,TIM_GP16,TRG,TIM1_TRG_COM_TIM17));
        __foreach_interrupt_inner!((TIM17,timer,TIM_GP16,UP,TIM1_TRG_COM_TIM17));
        __foreach_interrupt_inner!((TIM2,timer,TIM_GP32,BRK,TIM2));
        __foreach_interrupt_inner!((TIM2,timer,TIM_GP32,CC,TIM2));
        __foreach_interrupt_inner!((TIM2,timer,TIM_GP32,COM,TIM2));
        __foreach_interrupt_inner!((TIM2,timer,TIM_GP32,TRG,TIM2));
        __foreach_interrupt_inner!((TIM2,timer,TIM_GP32,UP,TIM2));
        __foreach_interrupt_inner!((TIM3,timer,TIM_GP16,BRK,TIM3));
        __foreach_interrupt_inner!((TIM3,timer,TIM_GP16,CC,TIM3));
        __foreach_interrupt_inner!((TIM3,timer,TIM_GP16,COM,TIM3));
        __foreach_interrupt_inner!((TIM3,timer,TIM_GP16,TRG,TIM3));
        __foreach_interrupt_inner!((TIM3,timer,TIM_GP16,UP,TIM3));
        __foreach_interrupt_inner!((TIM4,timer,TIM_GP16,BRK,TIM4));
        __foreach_interrupt_inner!((TIM4,timer,TIM_GP16,CC,TIM4));
        __foreach_interrupt_inner!((TIM4,timer,TIM_GP16,COM,TIM4));
        __foreach_interrupt_inner!((TIM4,timer,TIM_GP16,TRG,TIM4));
        __foreach_interrupt_inner!((TIM4,timer,TIM_GP16,UP,TIM4));
        __foreach_interrupt_inner!((TIM6,timer,TIM_BASIC,BRK,TIM6_DAC));
        __foreach_interrupt_inner!((TIM6,timer,TIM_BASIC,CC,TIM6_DAC));
        __foreach_interrupt_inner!((TIM6,timer,TIM_BASIC,COM,TIM6_DAC));
        __foreach_interrupt_inner!((TIM6,timer,TIM_BASIC,TRG,TIM6_DAC));
        __foreach_interrupt_inner!((TIM6,timer,TIM_BASIC,UP,TIM6_DAC));
        __foreach_interrupt_inner!((TIM7,timer,TIM_BASIC,BRK,TIM7));
        __foreach_interrupt_inner!((TIM7,timer,TIM_BASIC,CC,TIM7));
        __foreach_interrupt_inner!((TIM7,timer,TIM_BASIC,COM,TIM7));
        __foreach_interrupt_inner!((TIM7,timer,TIM_BASIC,TRG,TIM7));
        __foreach_interrupt_inner!((TIM7,timer,TIM_BASIC,UP,TIM7));
        __foreach_interrupt_inner!((TIM8,timer,TIM_ADV,BRK,TIM8_BRK));
        __foreach_interrupt_inner!((TIM8,timer,TIM_ADV,CC,TIM8_CC));
        __foreach_interrupt_inner!((TIM8,timer,TIM_ADV,COM,TIM8_TRG_COM));
        __foreach_interrupt_inner!((TIM8,timer,TIM_ADV,TRG,TIM8_TRG_COM));
        __foreach_interrupt_inner!((TIM8,timer,TIM_ADV,UP,TIM8_UP));
        __foreach_interrupt_inner!((UCPD1,ucpd,UCPD,GLOBAL,UCPD1));
        __foreach_interrupt_inner!((USART1,usart,USART,GLOBAL,USART1));
        __foreach_interrupt_inner!((USART2,usart,USART,GLOBAL,USART2));
        __foreach_interrupt_inner!((USART3,usart,USART,GLOBAL,USART3));
        __foreach_interrupt_inner!((USB,usb,USB,HP,USB_HP));
        __foreach_interrupt_inner!((USB,usb,USB,LP,USB_LP));
        __foreach_interrupt_inner!((USB,usb,USB,WKUP,USBWAKEUP));
        __foreach_interrupt_inner!((WWDG,wwdg,WWDG,GLOBAL,WWDG));
        __foreach_interrupt_inner!((WWDG,wwdg,WWDG,RST,WWDG));
        __foreach_interrupt_inner!((WWDG));
        __foreach_interrupt_inner!((PVD_PVM));
        __foreach_interrupt_inner!((RTC_TAMP_LSECSS));
        __foreach_interrupt_inner!((RTC_WKUP));
        __foreach_interrupt_inner!((FLASH));
        __foreach_interrupt_inner!((RCC));
        __foreach_interrupt_inner!((EXTI0));
        __foreach_interrupt_inner!((EXTI,EXTI0));
        __foreach_interrupt_inner!((EXTI1));
        __foreach_interrupt_inner!((EXTI,EXTI1));
        __foreach_interrupt_inner!((EXTI2));
        __foreach_interrupt_inner!((EXTI,EXTI2));
        __foreach_interrupt_inner!((EXTI3));
        __foreach_interrupt_inner!((EXTI,EXTI3));
        __foreach_interrupt_inner!((EXTI4));
        __foreach_interrupt_inner!((EXTI,EXTI4));
        __foreach_interrupt_inner!((DMA1_CHANNEL1));
        __foreach_interrupt_inner!((DMA1_CHANNEL2));
        __foreach_interrupt_inner!((DMA1_CHANNEL3));
        __foreach_interrupt_inner!((DMA1_CHANNEL4));
        __foreach_interrupt_inner!((DMA1_CHANNEL5));
        __foreach_interrupt_inner!((DMA1_CHANNEL6));
        __foreach_interrupt_inner!((ADC1_2));
        __foreach_interrupt_inner!((USB_HP));
        __foreach_interrupt_inner!((USB_LP));
        __foreach_interrupt_inner!((FDCAN1_IT0));
        __foreach_interrupt_inner!((FDCAN1_IT1));
        __foreach_interrupt_inner!((EXTI9_5));
        __foreach_interrupt_inner!((EXTI,EXTI9_5));
        __foreach_interrupt_inner!((TIM1_BRK_TIM15));
        __foreach_interrupt_inner!((TIM1_UP_TIM16));
        __foreach_interrupt_inner!((TIM1_TRG_COM_TIM17));
        __foreach_interrupt_inner!((TIM1_CC));
        __foreach_interrupt_inner!((TIM2));
        __foreach_interrupt_inner!((TIM3));
        __foreach_interrupt_inner!((TIM4));
        __foreach_interrupt_inner!((I2C1_EV));
        __foreach_interrupt_inner!((I2C1_ER));
        __foreach_interrupt_inner!((I2C2_EV));
        __foreach_interrupt_inner!((I2C2_ER));
        __foreach_interrupt_inner!((SPI1));
        __foreach_interrupt_inner!((SPI2));
        __foreach_interrupt_inner!((USART1));
        __foreach_interrupt_inner!((USART2));
        __foreach_interrupt_inner!((USART3));
        __foreach_interrupt_inner!((EXTI15_10));
        __foreach_interrupt_inner!((EXTI,EXTI15_10));
        __foreach_interrupt_inner!((RTC_ALARM));
        __foreach_interrupt_inner!((USBWAKEUP));
        __foreach_interrupt_inner!((TIM8_BRK));
        __foreach_interrupt_inner!((TIM8_UP));
        __foreach_interrupt_inner!((TIM8_TRG_COM));
        __foreach_interrupt_inner!((TIM8_CC));
        __foreach_interrupt_inner!((LPTIM1));
        __foreach_interrupt_inner!((SPI3));
        __foreach_interrupt_inner!((UART4));
        __foreach_interrupt_inner!((TIM6_DAC));
        __foreach_interrupt_inner!((TIM7));
        __foreach_interrupt_inner!((DMA2_CHANNEL1));
        __foreach_interrupt_inner!((DMA2_CHANNEL2));
        __foreach_interrupt_inner!((DMA2_CHANNEL3));
        __foreach_interrupt_inner!((DMA2_CHANNEL4));
        __foreach_interrupt_inner!((DMA2_CHANNEL5));
        __foreach_interrupt_inner!((UCPD1));
        __foreach_interrupt_inner!((COMP1_2_3));
        __foreach_interrupt_inner!((COMP4));
        __foreach_interrupt_inner!((CRS));
        __foreach_interrupt_inner!((SAI1));
        __foreach_interrupt_inner!((FPU));
        __foreach_interrupt_inner!((RNG));
        __foreach_interrupt_inner!((LPUART1));
        __foreach_interrupt_inner!((I2C3_EV));
        __foreach_interrupt_inner!((I2C3_ER));
        __foreach_interrupt_inner!((DMAMUX_OVR));
        __foreach_interrupt_inner!((DMA2_CHANNEL6));
        __foreach_interrupt_inner!((CORDIC));
        __foreach_interrupt_inner!((FMAC));
    };
}#[allow(unused)]
macro_rules! foreach_peripheral {
    ($($pat:tt => $code:tt;)*) => {
        macro_rules! __foreach_peripheral_inner {
            $(($pat) => $code;)*
            ($_:tt) => {}
        }
        __foreach_peripheral_inner!((adc,ADC1));
        __foreach_peripheral_inner!((adc,ADC2));
        __foreach_peripheral_inner!((adccommon,ADC_COMMON));
        __foreach_peripheral_inner!((cordic,CORDIC));
        __foreach_peripheral_inner!((crc,CRC));
        __foreach_peripheral_inner!((crs,CRS));
        __foreach_peripheral_inner!((dac,DAC1));
        __foreach_peripheral_inner!((dac,DAC3));
        __foreach_peripheral_inner!((dbgmcu,DBGMCU));
        __foreach_peripheral_inner!((bdma,DMA1));
        __foreach_peripheral_inner!((bdma,DMA2));
        __foreach_peripheral_inner!((dmamux,DMAMUX1));
        __foreach_peripheral_inner!((exti,EXTI));
        __foreach_peripheral_inner!((can,FDCAN1));
        __foreach_peripheral_inner!((fdcanram,FDCANRAM1));
        __foreach_peripheral_inner!((flash,FLASH));
        __foreach_peripheral_inner!((fmac,FMAC));
        __foreach_peripheral_inner!((gpio,GPIOA));
        __foreach_peripheral_inner!((gpio,GPIOB));
        __foreach_peripheral_inner!((gpio,GPIOC));
        __foreach_peripheral_inner!((gpio,GPIOD));
        __foreach_peripheral_inner!((gpio,GPIOE));
        __foreach_peripheral_inner!((gpio,GPIOF));
        __foreach_peripheral_inner!((gpio,GPIOG));
        __foreach_peripheral_inner!((i2c,I2C1));
        __foreach_peripheral_inner!((i2c,I2C2));
        __foreach_peripheral_inner!((i2c,I2C3));
        __foreach_peripheral_inner!((iwdg,IWDG));
        __foreach_peripheral_inner!((usart,LPUART1));
        __foreach_peripheral_inner!((opamp,OPAMP1));
        __foreach_peripheral_inner!((opamp,OPAMP2));
        __foreach_peripheral_inner!((opamp,OPAMP3));
        __foreach_peripheral_inner!((pwr,PWR));
        __foreach_peripheral_inner!((rcc,RCC));
        __foreach_peripheral_inner!((rng,RNG));
        __foreach_peripheral_inner!((rtc,RTC));
        __foreach_peripheral_inner!((sai,SAI1));
        __foreach_peripheral_inner!((spi,SPI1));
        __foreach_peripheral_inner!((spi,SPI2));
        __foreach_peripheral_inner!((spi,SPI3));
        __foreach_peripheral_inner!((syscfg,SYSCFG));
        __foreach_peripheral_inner!((tamp,TAMP));
        __foreach_peripheral_inner!((timer,TIM1));
        __foreach_peripheral_inner!((timer,TIM15));
        __foreach_peripheral_inner!((timer,TIM16));
        __foreach_peripheral_inner!((timer,TIM17));
        __foreach_peripheral_inner!((timer,TIM2));
        __foreach_peripheral_inner!((timer,TIM3));
        __foreach_peripheral_inner!((timer,TIM4));
        __foreach_peripheral_inner!((timer,TIM6));
        __foreach_peripheral_inner!((timer,TIM7));
        __foreach_peripheral_inner!((timer,TIM8));
        __foreach_peripheral_inner!((ucpd,UCPD1));
        __foreach_peripheral_inner!((uid,UID));
        __foreach_peripheral_inner!((usart,USART1));
        __foreach_peripheral_inner!((usart,USART2));
        __foreach_peripheral_inner!((usart,USART3));
        __foreach_peripheral_inner!((usb,USB));
        __foreach_peripheral_inner!((usbram,USBRAM));
        __foreach_peripheral_inner!((wwdg,WWDG));
    };
}#[allow(unused)]
macro_rules! foreach_pin {
    ($($pat:tt => $code:tt;)*) => {
        macro_rules! __foreach_pin_inner {
            $(($pat) => $code;)*
            ($_:tt) => {}
        }
        __foreach_pin_inner!((PA0,GPIOA,0,0,EXTI0));
        __foreach_pin_inner!((PA1,GPIOA,0,1,EXTI1));
        __foreach_pin_inner!((PA2,GPIOA,0,2,EXTI2));
        __foreach_pin_inner!((PA3,GPIOA,0,3,EXTI3));
        __foreach_pin_inner!((PA4,GPIOA,0,4,EXTI4));
        __foreach_pin_inner!((PA5,GPIOA,0,5,EXTI5));
        __foreach_pin_inner!((PA6,GPIOA,0,6,EXTI6));
        __foreach_pin_inner!((PA7,GPIOA,0,7,EXTI7));
        __foreach_pin_inner!((PA8,GPIOA,0,8,EXTI8));
        __foreach_pin_inner!((PA9,GPIOA,0,9,EXTI9));
        __foreach_pin_inner!((PA10,GPIOA,0,10,EXTI10));
        __foreach_pin_inner!((PA11,GPIOA,0,11,EXTI11));
        __foreach_pin_inner!((PA12,GPIOA,0,12,EXTI12));
        __foreach_pin_inner!((PA13,GPIOA,0,13,EXTI13));
        __foreach_pin_inner!((PA14,GPIOA,0,14,EXTI14));
        __foreach_pin_inner!((PA15,GPIOA,0,15,EXTI15));
        __foreach_pin_inner!((PB0,GPIOB,1,0,EXTI0));
        __foreach_pin_inner!((PB1,GPIOB,1,1,EXTI1));
        __foreach_pin_inner!((PB2,GPIOB,1,2,EXTI2));
        __foreach_pin_inner!((PB3,GPIOB,1,3,EXTI3));
        __foreach_pin_inner!((PB4,GPIOB,1,4,EXTI4));
        __foreach_pin_inner!((PB5,GPIOB,1,5,EXTI5));
        __foreach_pin_inner!((PB6,GPIOB,1,6,EXTI6));
        __foreach_pin_inner!((PB7,GPIOB,1,7,EXTI7));
        __foreach_pin_inner!((PB8,GPIOB,1,8,EXTI8));
        __foreach_pin_inner!((PB9,GPIOB,1,9,EXTI9));
        __foreach_pin_inner!((PB10,GPIOB,1,10,EXTI10));
        __foreach_pin_inner!((PB11,GPIOB,1,11,EXTI11));
        __foreach_pin_inner!((PB12,GPIOB,1,12,EXTI12));
        __foreach_pin_inner!((PB13,GPIOB,1,13,EXTI13));
        __foreach_pin_inner!((PB14,GPIOB,1,14,EXTI14));
        __foreach_pin_inner!((PB15,GPIOB,1,15,EXTI15));
        __foreach_pin_inner!((PC0,GPIOC,2,0,EXTI0));
        __foreach_pin_inner!((PC1,GPIOC,2,1,EXTI1));
        __foreach_pin_inner!((PC2,GPIOC,2,2,EXTI2));
        __foreach_pin_inner!((PC3,GPIOC,2,3,EXTI3));
        __foreach_pin_inner!((PC4,GPIOC,2,4,EXTI4));
        __foreach_pin_inner!((PC5,GPIOC,2,5,EXTI5));
        __foreach_pin_inner!((PC6,GPIOC,2,6,EXTI6));
        __foreach_pin_inner!((PC7,GPIOC,2,7,EXTI7));
        __foreach_pin_inner!((PC8,GPIOC,2,8,EXTI8));
        __foreach_pin_inner!((PC9,GPIOC,2,9,EXTI9));
        __foreach_pin_inner!((PC10,GPIOC,2,10,EXTI10));
        __foreach_pin_inner!((PC11,GPIOC,2,11,EXTI11));
        __foreach_pin_inner!((PC12,GPIOC,2,12,EXTI12));
        __foreach_pin_inner!((PC13,GPIOC,2,13,EXTI13));
        __foreach_pin_inner!((PC14,GPIOC,2,14,EXTI14));
        __foreach_pin_inner!((PC15,GPIOC,2,15,EXTI15));
        __foreach_pin_inner!((PD0,GPIOD,3,0,EXTI0));
        __foreach_pin_inner!((PD1,GPIOD,3,1,EXTI1));
        __foreach_pin_inner!((PD2,GPIOD,3,2,EXTI2));
        __foreach_pin_inner!((PD3,GPIOD,3,3,EXTI3));
        __foreach_pin_inner!((PD4,GPIOD,3,4,EXTI4));
        __foreach_pin_inner!((PD5,GPIOD,3,5,EXTI5));
        __foreach_pin_inner!((PD6,GPIOD,3,6,EXTI6));
        __foreach_pin_inner!((PD7,GPIOD,3,7,EXTI7));
        __foreach_pin_inner!((PD8,GPIOD,3,8,EXTI8));
        __foreach_pin_inner!((PD9,GPIOD,3,9,EXTI9));
        __foreach_pin_inner!((PD10,GPIOD,3,10,EXTI10));
        __foreach_pin_inner!((PD11,GPIOD,3,11,EXTI11));
        __foreach_pin_inner!((PD12,GPIOD,3,12,EXTI12));
        __foreach_pin_inner!((PD13,GPIOD,3,13,EXTI13));
        __foreach_pin_inner!((PD14,GPIOD,3,14,EXTI14));
        __foreach_pin_inner!((PD15,GPIOD,3,15,EXTI15));
        __foreach_pin_inner!((PE0,GPIOE,4,0,EXTI0));
        __foreach_pin_inner!((PE1,GPIOE,4,1,EXTI1));
        __foreach_pin_inner!((PE2,GPIOE,4,2,EXTI2));
        __foreach_pin_inner!((PE3,GPIOE,4,3,EXTI3));
        __foreach_pin_inner!((PE4,GPIOE,4,4,EXTI4));
        __foreach_pin_inner!((PE5,GPIOE,4,5,EXTI5));
        __foreach_pin_inner!((PE6,GPIOE,4,6,EXTI6));
        __foreach_pin_inner!((PE7,GPIOE,4,7,EXTI7));
        __foreach_pin_inner!((PE8,GPIOE,4,8,EXTI8));
        __foreach_pin_inner!((PE9,GPIOE,4,9,EXTI9));
        __foreach_pin_inner!((PE10,GPIOE,4,10,EXTI10));
        __foreach_pin_inner!((PE11,GPIOE,4,11,EXTI11));
        __foreach_pin_inner!((PE12,GPIOE,4,12,EXTI12));
        __foreach_pin_inner!((PE13,GPIOE,4,13,EXTI13));
        __foreach_pin_inner!((PE14,GPIOE,4,14,EXTI14));
        __foreach_pin_inner!((PE15,GPIOE,4,15,EXTI15));
        __foreach_pin_inner!((PF0,GPIOF,5,0,EXTI0));
        __foreach_pin_inner!((PF1,GPIOF,5,1,EXTI1));
        __foreach_pin_inner!((PF2,GPIOF,5,2,EXTI2));
        __foreach_pin_inner!((PF3,GPIOF,5,3,EXTI3));
        __foreach_pin_inner!((PF4,GPIOF,5,4,EXTI4));
        __foreach_pin_inner!((PF5,GPIOF,5,5,EXTI5));
        __foreach_pin_inner!((PF6,GPIOF,5,6,EXTI6));
        __foreach_pin_inner!((PF7,GPIOF,5,7,EXTI7));
        __foreach_pin_inner!((PF8,GPIOF,5,8,EXTI8));
        __foreach_pin_inner!((PF9,GPIOF,5,9,EXTI9));
        __foreach_pin_inner!((PF10,GPIOF,5,10,EXTI10));
        __foreach_pin_inner!((PF11,GPIOF,5,11,EXTI11));
        __foreach_pin_inner!((PF12,GPIOF,5,12,EXTI12));
        __foreach_pin_inner!((PF13,GPIOF,5,13,EXTI13));
        __foreach_pin_inner!((PF14,GPIOF,5,14,EXTI14));
        __foreach_pin_inner!((PF15,GPIOF,5,15,EXTI15));
        __foreach_pin_inner!((PG0,GPIOG,6,0,EXTI0));
        __foreach_pin_inner!((PG1,GPIOG,6,1,EXTI1));
        __foreach_pin_inner!((PG2,GPIOG,6,2,EXTI2));
        __foreach_pin_inner!((PG3,GPIOG,6,3,EXTI3));
        __foreach_pin_inner!((PG4,GPIOG,6,4,EXTI4));
        __foreach_pin_inner!((PG5,GPIOG,6,5,EXTI5));
        __foreach_pin_inner!((PG6,GPIOG,6,6,EXTI6));
        __foreach_pin_inner!((PG7,GPIOG,6,7,EXTI7));
        __foreach_pin_inner!((PG8,GPIOG,6,8,EXTI8));
        __foreach_pin_inner!((PG9,GPIOG,6,9,EXTI9));
        __foreach_pin_inner!((PG10,GPIOG,6,10,EXTI10));
        __foreach_pin_inner!((PG11,GPIOG,6,11,EXTI11));
        __foreach_pin_inner!((PG12,GPIOG,6,12,EXTI12));
        __foreach_pin_inner!((PG13,GPIOG,6,13,EXTI13));
        __foreach_pin_inner!((PG14,GPIOG,6,14,EXTI14));
        __foreach_pin_inner!((PG15,GPIOG,6,15,EXTI15));
    };
}#[allow(unused)]
macro_rules! foreach_dma_channel {
    ($($pat:tt => $code:tt;)*) => {
        macro_rules! __foreach_dma_channel_inner {
            $(($pat) => $code;)*
            ($_:tt) => {}
        }
        __foreach_dma_channel_inner!((DMA1_CH1,DMA1,bdma,0,0,{dmamux: DMAMUX1, dmamux_channel: 0}));
        __foreach_dma_channel_inner!((DMA1_CH2,DMA1,bdma,1,1,{dmamux: DMAMUX1, dmamux_channel: 1}));
        __foreach_dma_channel_inner!((DMA1_CH3,DMA1,bdma,2,2,{dmamux: DMAMUX1, dmamux_channel: 2}));
        __foreach_dma_channel_inner!((DMA1_CH4,DMA1,bdma,3,3,{dmamux: DMAMUX1, dmamux_channel: 3}));
        __foreach_dma_channel_inner!((DMA1_CH5,DMA1,bdma,4,4,{dmamux: DMAMUX1, dmamux_channel: 4}));
        __foreach_dma_channel_inner!((DMA1_CH6,DMA1,bdma,5,5,{dmamux: DMAMUX1, dmamux_channel: 5}));
        __foreach_dma_channel_inner!((DMA1_CH7,DMA1,bdma,6,6,{dmamux: DMAMUX1, dmamux_channel: 6}));
        __foreach_dma_channel_inner!((DMA1_CH8,DMA1,bdma,7,7,{dmamux: DMAMUX1, dmamux_channel: 7}));
        __foreach_dma_channel_inner!((DMA2_CH1,DMA2,bdma,0,8,{dmamux: DMAMUX1, dmamux_channel: 8}));
        __foreach_dma_channel_inner!((DMA2_CH2,DMA2,bdma,1,9,{dmamux: DMAMUX1, dmamux_channel: 9}));
        __foreach_dma_channel_inner!((DMA2_CH3,DMA2,bdma,2,10,{dmamux: DMAMUX1, dmamux_channel: 10}));
        __foreach_dma_channel_inner!((DMA2_CH4,DMA2,bdma,3,11,{dmamux: DMAMUX1, dmamux_channel: 11}));
        __foreach_dma_channel_inner!((DMA2_CH5,DMA2,bdma,4,12,{dmamux: DMAMUX1, dmamux_channel: 12}));
        __foreach_dma_channel_inner!((DMA2_CH6,DMA2,bdma,5,13,{dmamux: DMAMUX1, dmamux_channel: 13}));
        __foreach_dma_channel_inner!((DMA2_CH7,DMA2,bdma,6,14,{dmamux: DMAMUX1, dmamux_channel: 14}));
        __foreach_dma_channel_inner!((DMA2_CH8,DMA2,bdma,7,15,{dmamux: DMAMUX1, dmamux_channel: 15}));
    };
}#[allow(unused)]
macro_rules! foreach_adc {
    ($($pat:tt => $code:tt;)*) => {
        macro_rules! __foreach_adc_inner {
            $(($pat) => $code;)*
            ($_:tt) => {}
        }
        __foreach_adc_inner!((ADC1,ADC_COMMON,adc));
        __foreach_adc_inner!((ADC2,ADC_COMMON,adc));
    };
}