#![no_std]
#![no_main]

use pd_charger::prelude::*;

use lilos::{self, time::Millis};

use hal::{gpio, i2c};

// I2C implementation adapted from https://github.com/embassy-rs/embassy/blob/main/examples/stm32g0/src/bin/i2c_async.rs

// allow the trait-impl-incorrect-safety lint for the `bind_interrupts!` macro

hal::bind_interrupts!(struct Irqs {
    // Interrupt names found here
    // https://docs.embassy.dev/stm32-metapac/git/stm32g431cb/enum.Interrupt.html
    // I2C Event Interrupt
    I2C1_EV => i2c::EventInterruptHandler<hal::peripherals::I2C1>;
    // I2C Error Interrupt
    I2C1_ER => i2c::ErrorInterruptHandler<hal::peripherals::I2C1>;
});

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

    let fusb_i2c_config = {
        let mut c = i2c::Config::default();
        c.scl_pullup = true;
        c.sda_pullup = true;
        c.timeout = embassy_time::Duration::from_millis(10);
        c
    };

    // !!ERROR!! setting the I2C1 peripheral speed too high can cause hard-faults (interrupts)
    // if the required perihperal speed owuld then exceed the core/source clock speed
    let mut fusb_i2c_port = i2c::I2c::new(
        p.I2C1,
        p.PB8,
        p.PB7,
        Irqs,
        p.DMA1_CH1,
        p.DMA1_CH2,
        hal::time::Hertz(100_000),
        fusb_i2c_config,
    );

    // --- task(s) setup ---

    let blink_task = pin!(blink(&mut loaden_pin));

    let i2c_scan_task = pin!(i2c_scan(&mut fusb_i2c_port, Millis(5000)));

    // --- runtime setup and start ---

    lilos::time::initialize_sys_tick(&mut cp.SYST, CLOCK_FREQ);

    lilos::exec::run_tasks(&mut [blink_task, i2c_scan_task], lilos::exec::ALL_TASKS)
}

async fn blink<SomePin>(arg: &mut SomePin) -> Infallible
where
    SomePin: embedded_hal::digital::OutputPin,
{
    loop {
        arg.set_high().unwrap();
        // info!("set high");
        lilos::time::sleep_for(Millis(1000)).await;

        arg.set_low().unwrap();
        // info!("set low");
        lilos::time::sleep_for(Millis(1000)).await;
    }
}

/// Scans the I2C bus for devices `once_every_n` milliseconds and prints the addresses of any devices found to via rtt.
async fn i2c_scan<SomeI2c>(i2c: &mut SomeI2c, once_every_n: Millis) -> Infallible
where
    SomeI2c: embedded_hal_async::i2c::I2c,
{
    let mut gate = lilos::time::PeriodicGate::from(once_every_n);

    lilos::exec::yield_cpu().await;

    loop {
        info!("Scanning I2C bus...");

        let mut n_found = 0_u8;
        for addr in 1..126 {
            // info!("Checking address 0x{:02X}", addr);
            match i2c.read(addr, &mut [0u8]).await {
                Ok(_) => {
                    info!("Found device at address 0x{:02X}", addr);
                    n_found += 1;
                }
                Err(_) => {}
            }
        }

        if n_found == 0 {
            info!("No devices found");
        }

        gate.next_time().await;
    }
}
