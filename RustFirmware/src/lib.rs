#![no_std]

// use pub to make this accesible to other files

pub mod prelude {
    #![allow(unused_imports)]

    // lilos and other async stuff
    pub use core::{convert::Infallible, pin::pin};
    pub use lilos;

    // hal and stm32g431 specific peripherals/etc
    pub use embassy_stm32 as hal;

    // rtt setup and rtt print macro
    pub use defmt::info;
    pub use defmt_rtt as _;

    // panic handler
    pub use panic_probe as _;

    // ARM Cortex-M4 specific peripheral access
    pub use cortex_m_rt;
}
