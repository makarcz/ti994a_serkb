Hardware:
    Atmel 8051 compatible microcontroller AT89S52.
    TI99-4A matrix keyboard (ports P2, P3).
       See: http://www.hardwarebook.info/TI-99/4A_Keyboard
    Serial line drivers (NPN transistors, base and pull-up resistors, NAND gates 74LS00).
    NAND gates - incoming data, clock from serial bus to data/clock in pins.
    NPN transistors - outgoing data, clock signals from 8051 data/clock out pins to serial bus.

Compiler: SDCC 3.6.0

Theory of operation:

   Firmware in microcontroller scans the matrix alphanumeric keyboard and transmits
   the byte representation of the keystrokes over the clocked serial interface, similar
   electrically to I2C.

  NOTE: This code is adapted from my previous I2C keyboard interface
        version. The protocol was simplified for clocked serial output only
        and doesn't follow I2C protocol. So only point-to-point connection,
        keyboard device is an output/sender, a computer or other micro is an
        input/receiver. It is NOT a bus.
        Beware that some symbols in code may be named similarly to I2C bus
        signals or suggest I2C protocol implementation - they're not the I2C
        though, just my own serial protocol, which is as follows:

        START SEQUENCE (as in I2C) (SDA=SCL=1, SDA=SCL=0)
        8-bits of data on SDA clocked by SCL.

        NOTE: There are no stop bits. There are no ACK responses.
              One way communication only.
              Each state/logic level is at least 4 ms wide.
              All that receiver needs to do is to detect start sequence
              and then at each clock pulse shift-in the data bits.

              Example SPIN code for Parallax Propeller:

              PRI get_byte | b, l

                b := 0
                repeat 8
                   'detect clock pulse SCL
                   repeat while ina[SCL_pin] == 1   ' or 0
                   repeat while ina[SCL_pin] == 0   ' or 1, both ways work
                                                    ' since pulse is slow
                   b := (b << 1) | ina[SDA_pin]      ' read the bit

                return (b & $FF)
 
 ----------------------------------------------------------------------------

  SDA  ||||||||________XXXXXXXXXXXXXXXX...XXXXXXXXXXXXXXXX||||||||________...
  SCL  ||||||||________||||||||________...||||||||________||||||||________...

       <-4ms -><-4ms -><-4ms -><-4ms ->...<-4ms -><-4ms -><-4ms -><-4ms ->...
       <- START SEQ. -><-------  8-bits of DATA   -------><- START SEQ. ->...
