#![no_std]
#![no_main]

use pd_charger::prelude::*;

use lilos::{self, time::Millis};

use hal::gpio;

#[cortex_m_rt::entry]
fn main() -> ! {
    // cp = core peripherals
    let mut cp = cortex_m::Peripherals::take().unwrap();

    // p = peripherals
    let p: hal::Peripherals = hal::init(hal::Config::default());
    const CLOCK_FREQ: u32 = 16_000_000;

    // --- hardware setup --
    // TODO: may be worth moving some of this (or this function) into the shared.rs

    let mut loaden_pin = gpio::Output::new(p.PC11, gpio::Level::High, gpio::Speed::Low);

    // --- task(s) setup ---

    let blink_task = pin!(blink(&mut loaden_pin));

    // --- runtime setup and start ---

    lilos::time::initialize_sys_tick(&mut cp.SYST, CLOCK_FREQ);

    lilos::exec::run_tasks(&mut [blink_task], lilos::exec::ALL_TASKS)
}

async fn blink<SomePin>(arg: &mut SomePin) -> Infallible
where
    SomePin: embedded_hal::digital::OutputPin,
{
    loop {
        arg.set_high().unwrap();
        info!("set high");
        lilos::time::sleep_for(Millis(1000)).await;

        arg.set_low().unwrap();
        info!("set low");
        lilos::time::sleep_for(Millis(1000)).await;
    }
}
