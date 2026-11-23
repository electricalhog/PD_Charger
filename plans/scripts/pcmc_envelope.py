# Peak-current-mode envelope on the board as built (model). Reset level = DAC value at the ramp start, in amps above IL_MON's zero.
L=4.7e-6; VIN=24.6; ETA=0.93; SENS=0.100; CNT=4096/3.3; ZERO=1.640; IMAX=(3.3-ZERO)/SENS; TAU=200e-9; NOTCH=640e-9
pts=[(5,3),(9,3),(15,3),(20,5),(24,5),(28,5),(36,5),(48,5)]
print(f"span {IMAX:.1f} A, {SENS*CNT:.0f} counts/A, tau {TAU*1e9:.0f} ns (assumed)")
print("Vout Iout kind   kHz   D    IL_avg dI_pp  I_pk  m1    m2   m_a   ramp(A) reset(A) step(cnt, 64|32 per T)  I_out at CCM boundary")
for vo,io in pts:
    kind='buck' if vo<VIN*0.9 else ('band' if vo<VIN*1.03 else 'boost')
    for f in (200e3,400e3):
        T=1/f; N=64 if f==200e3 else 32
        if kind=='buck':
            D=vo/(VIN*ETA**0.5); ILa=io; m1=(VIN-vo)/L; m2=vo/L
        elif kind=='band':   # Q2 notch then Q4 boost pulse: L sees Vin while Q4 on, -(Vout-Vin) after, -Vout in the notch
            D=max(1-VIN*ETA**0.5*(1-NOTCH*f)/vo,0.03); ILa=io*vo/VIN/ETA; m1=VIN/L; m2=max(vo-VIN,0.2)/L
        else:
            D=1-VIN*ETA**0.5/vo; ILa=io*vo/VIN/ETA; m1=VIN/L; m2=(vo-VIN)/L
        ton=D*T; dI=m1*ton; ipk=ILa+dI/2
        ma=max(0.0,(m2-m1)/2)*1.25 if D>0.45 else 0.0
        ramp=ma*ton; reset=ipk-m1*TAU+ramp
        step=ma*(T/N)*SENS*CNT*16   # 12.4 fixed point
        ib=dI/2 if kind=='buck' else dI/2*(1-D)
        flag='  OVER' if reset>IMAX-0.5 or ipk>IMAX-1 else ''
        print(f"{vo:4} {io:4} {kind:5} {f/1e3:5.0f} {D:5.2f} {ILa:6.2f} {dI:5.2f} {ipk:6.2f} {m1/1e6:5.2f} {m2/1e6:5.2f} {ma/1e6:5.2f} {ramp:6.2f} {reset:7.2f}{flag:6} {step:7.0f}             {ib:5.2f}")
