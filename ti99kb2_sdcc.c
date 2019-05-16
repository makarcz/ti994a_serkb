/*
 * Project: TI99-4A retro serial keyboard.
 * Module:  ti994a_2
 * Author:  Marek Karcz
 * Updated: 2016/10/11
 * Purpose: TI99-4A keyboard, serial I/O device.
 * 
 * Hardware:
 *    Atmel 8051 compatible microcontroller AT89S52.
 *    TI99-4A matrix keyboard (ports P2, P3).
 *       See: http://www.hardwarebook.info/TI-99/4A_Keyboard
 *    Serial line drivers (NPN transistors, base and pull-up resistors, NAND gates 74LS00).
 *    NAND gates - incoming data, clock from serial bus to data/clock in pins.
 *    NPN transistors - outgoing data, clock signals from 8051 data/clock out pins to serial bus.
 *
 * GVIM: set tabstop=3 shiftwidth=3 expandtab
 * Compiler: SDCC 3.6.0
 *
 * Keyboard to port pin connections:
 *
 * TI99-4a pin#      Pn.b
 * --------------------------
 *          11       P3.7
 *          10       P3.6
 *           3       P3.5
 *           7       P3.4
 *           2       P3.3
 *           1       P3.2
 *           4       P3.1
 *           5       P3.0
 *
 *           6       P2.6
 *           8       P2.5
 *           9       P2.4
 *          15       P2.3
 *          14       P2.2
 *          13       P2.1
 *          12       P2.0
 *
 * =================================
 *
 *  Serial bus interface:
 *
 *  P1.3 - data out inverted
 *  P1.4 - data in  inverted
 *  P1.5 - clock out inverted
 *  P1.6 - clock in  inverted
 *
 *  ================================
 *
 *  NOTE: This code is adapted from my previous I2C keyboard interface
 *        version. The protocol was simplified for clocked serial output only
 *        and doesn't follow I2C protocol. So only point-to-point connection,
 *        keyboard device is an output/sender, a computer or other micro is an
 *        input/receiver. It is NOT a bus.
 *        Beware that some symbols in code may be named similarly to I2C bus
 *        signals or suggest I2C protocol implementation - they're not the I2C
 *        though, just my own serial protocol, which is as follows:
 *
 *        START SEQUENCE (as in I2C) (SDA=SCL=1, SDA=SCL=0)
 *        8-bits of data on SDA clocked by SCL.
 *
 *        NOTE: There are no stop bits. There are no ACK responses.
 *              One way communication only.
 *              Each state/logic level is at least 4 ms wide.
 *              All that receiver needs to do is to detect start sequence
 *              and then at each clock pulse shift-in the data bits.
 *
 *              Example SPIN code for Parallax Propeller:
 *
 *              PRI get_byte | b, l
 *
 *                b := 0
 *                repeat 8
 *                   'detect clock pulse SCL
 *                   repeat while ina[SCL_pin] == 1   ' or 0
 *                   repeat while ina[SCL_pin] == 0   ' or 1, both ways work
 *                                                    ' since pulse is slow
 *                   b := (b << 1) | ina[SDA_pin]      ' read the bit
 *
 *                return (b & $FF)
 * 
 * ----------------------------------------------------------------------------
 *
 *  SDA  ||||||||________XXXXXXXXXXXXXXXX...XXXXXXXXXXXXXXXX||||||||________...
 *  SCL  ||||||||________||||||||________...||||||||________||||||||________...
 *
 *       <-4ms -><-4ms -><-4ms -><-4ms ->...<-4ms -><-4ms -><-4ms -><-4ms ->...
 *       <- START SEQ. -><-------  8-bits of DATA   -------><- START SEQ. ->...
 *  
 */


#include <at89x52.h>

// keil -> sdcc
#define sbit __sbit
#define code __code
#define using __using
#define interrupt __interrupt
//#define _nop_() __asm NOP __endasm
//#define _nop4_() __asm NOP NOP NOP NOP __endasm
typedef unsigned char BYTE;
typedef unsigned int WORD;
typedef sbit BOOL ;

#define rs         P1_0 
#define rw         P1_1
#define ep         P1_2
#define KBP1       P3
#define KBP2       P2

// NOTE: serial pins are inverted
// 
#define SDA_OUT    P1_3
#define SDA_IN     P1_4
#define SCL_OUT    P1_5
#define SCL_IN     P1_6
#define BUS_HIGH   0
#define BUS_LOW    1
#define TRUE       1
#define FALSE      0
#define KEYBUFLEN  10
#define SIG_DEL    4       // signal delay [ms]
// this may need to be fine tuned to avoid missing the
// key presses, can be longer for a slow typist, should be
// short for fast typist
#define KEYRD_DEL  115     // delay after key press [ms]

BYTE g_ucKeyCode=0;
char g_cKey=0;
BYTE g_ucRow=0;
BYTE g_ucColumn=0;
BYTE code g_ucaKbMatrix[8][8] =
{
	{11, 43, 42, 41, 40, 22,  0, 0},
	{47, 31, 30, 29, 28, 32,  0, 0},
	{33, 20, 19, 18, 17, 21,  0, 0},
	{ 0,  9,  8,  7,  6, 10,  0, 0},
	{48,  2,  3,  4,  5,  1, 45, 0},
	{44, 24, 25, 26, 27, 23,  0, 0},
	{46, 13, 14, 15, 16, 12,  0, 0},
	{ 0, 36, 37, 38, 39, 35,  0, 0}
};

BOOL g_bShiftOn = FALSE;
BOOL g_bCtrl = FALSE;
BOOL g_bFunc = FALSE;
BOOL g_bLock = FALSE;

BOOL g_bKeyHit = FALSE;

BYTE g_ucaKeyBuf[KEYBUFLEN];
BYTE g_ucKeyBufStartIndex = 0;// at the beginning buffer is empty
BYTE g_ucKeyBufEndIndex = 0;  // StartIndex == EndIndex -> buffer empty

/*
 * # of times per second the timer counter is incremented with
 * a 11,0592 MHz crystal:
 *
 * 11,059,200 / 12 = 921,600
 *
 * Looking for timer re-load value for interrupt occur 25 times per second.
 *
 * 1. How many increments until overflow? - x:
 *
 * 1 sec -> 921,600
 * 0.04 sec -> x?
 *
 * x = 921,600 * 0.04 = 36,864 = $9000
 *
 * 2. Overflow is when counter goes from $FFFF to 0.
 * 
 * Looking for reload value rel, where: $FFFF + 1 - rel = $9000
 *
 * rel = $FFFF + 1 - $9000 = $7000 
 *
 * Following above process, the reload value for 50 times per second:
 *
 * rel = $B800
 *
 * 1000 times per second (every 1 milisecond):
 *
 * rel = $FC66
 *
 * 20 times per second (every 0.05 s)
 *
 * rel = $4C00
 */

void init_SFR(void)
{
   // timer 0 - used to generate periodic interrupt for keyboard scan routine
   TR0   = 0;    // stop timer0
   TF0  =  0;    // clear timer0 overflow bit
   IE    = 0;    // disable interrupts
	// timer 2 - used to measure precise time, no interrupts
   TR2    = 0;    // timer2 doesn't run
   TF2    = 0;    // clear timer2 overflow bit
	CP_RL2 = 0; 	// auto-reload when timer overflows
	C_T2   = 0; 	// internal counter/timer
	EXEN2  = 0; 	// ignore pin T2EX
	TCLK   = 0; 	// not a baud clock
	RCLK   = 0; 	// not a baud clock
	RCAP2H = 0xFC;	// reload values for timer 2, 1 msec delay
	RCAP2L = 0x66;
	TH2    = 0xFC; 
	TL2    = 0x66;
}

void init_ports(void)
{
	KBP1 = 0xFF;   // P3 as keyboard output scan port
	KBP2 = 0xFF;   // P2 as keyboard input scan port
	P1 = 0xFF;
	SDA_IN = 1;
	SCL_IN = 1;
	SDA_OUT = 0;
	SCL_OUT = 0;
}

void ResetSpecKeysFlags()
{
   g_bShiftOn 	= FALSE;
   g_bLock 		= FALSE;
	g_bCtrl 		= FALSE;
	g_bFunc 		= FALSE;
}

void init_vars(void)
{
	BYTE i = 0;
	g_ucKeyCode=0xFF;
	g_ucRow=0;
	g_ucColumn=0;
	g_ucKeyBufStartIndex = g_ucKeyBufEndIndex = 0;
	g_bKeyHit = FALSE;
   ResetSpecKeysFlags();
	for (i=0; i<KEYBUFLEN; i++)
	{
		g_ucaKeyBuf[i] = 0;
	}
}

/*
 * Delay # of ms.
 * Uses timer 2.
 * NOTE: This routine accumulates error because
 *       it is not cycle optimized for overhead
 *       of setting registers and incrementing
 *       counter etc. The reload value $FC66
 *       is the perfect value with no oevrhead
 *       taken into consideration.
 */
void delms(unsigned int ms)
{
	unsigned int count = 0;

   if (ms > 0) {
	   TR2 = 1;	// run timer 2
	   while (count < ms) {
		   while (0==TF2) ;	// wait for TF2 (overflow) bit
		   TF2 = 0;				// clear overflow bit
		   count++;
	   }
	   TR2 = 0;	// stop timer 2
	   TF2 = 0;	// clear overflow bit
   }
}

/*
 * TI99-4A keyboard driver functions.
 */

/*
 * Convert keyboard scan code to ANSI ASCII code.
 * (more or less :-) )
 */
BYTE convKeyCode2Char(BYTE kc)
{
   BYTE ch = 0;

   switch (kc)
   {
      case  1: ch = ((g_bShiftOn) ? '!' : '1'); break; 
      case  2: ch = ((g_bShiftOn) ? '@' : '2'); break; 
      case  3: ch = ((g_bShiftOn) ? '#' : '3'); break; 
      case  4: ch = ((g_bShiftOn) ? '$' : '4'); break; 
      case  5: ch = ((g_bShiftOn) ? '%' : '5'); break; 
      case  6: ch = ((g_bShiftOn) ? '^' : '6'); break; 
      case  7: ch = ((g_bShiftOn) ? '&' : '7'); break; 
      case  8: ch = ((g_bShiftOn) ? '*' : '8'); break; 
      case  9: ch = ((g_bShiftOn) ? '(' : '9'); break; 
      case 10: ch = ((g_bShiftOn) ? ')' : '0'); break; 
      case 11: ch = ((g_bShiftOn) ? '+' : '='); break; 
      case 12: if (g_bCtrl) ch = 17; /* CTRL-Q */ else ch = ((g_bShiftOn) ? 'Q' : 'q'); break; 
      case 13: if (g_bFunc) ch = '~'; else ch = ((g_bShiftOn) ? 'W' : 'w'); break; 
      case 14: if (g_bFunc) ch = 128; /* up arrow */ else ch = ((g_bShiftOn) ? 'E' : 'e'); break; 
      case 15: if (g_bFunc) ch = '['; else ch = ((g_bShiftOn) ? 'R' : 'r'); break; 
      case 16: if (g_bFunc) ch = ']'; else ch = ((g_bShiftOn) ? 'T' : 't'); break; 
      case 17: ch = ((g_bShiftOn) ? 'Y' : 'y'); break; 
      case 18: if (g_bFunc) ch = '_'; else ch = ((g_bShiftOn) ? 'U' : 'u'); break; 
      case 19: if (g_bFunc) ch = '?'; else ch = ((g_bShiftOn) ? 'I' : 'i'); break; 
      case 20: if (g_bFunc) ch = '\''; else ch = ((g_bShiftOn) ? 'O' : 'o'); break; 
      case 21: if (g_bFunc) ch = '"'; else ch = ((g_bShiftOn) ? 'P' : 'p'); break; 
      case 22: ch = ((g_bShiftOn) ? '-' : '/'); break; 
      case 23: if (g_bFunc) ch = '|'; else ch = ((g_bShiftOn) ? 'A' : 'a'); break; 
      case 24: if (g_bCtrl) ch = 19; /* CTRL-S */
			   else {if (g_bFunc) ch = 129; /* left arrow */ else ch = ((g_bShiftOn) ? 'S' : 's');}
			   break; 
      case 25: if (g_bFunc) ch = 130; /* right arrow */ else ch = ((g_bShiftOn) ? 'D' : 'd'); break; 
      case 26: if (g_bFunc) ch = '{'; else ch = ((g_bShiftOn) ? 'F' : 'f'); break; 
      case 27: if (g_bFunc) ch = '}'; else ch = ((g_bShiftOn) ? 'G' : 'g'); break; 
      case 28: if (g_bCtrl) ch = 8; /* CTRL-H or BACKSPACE */ else ch = ((g_bShiftOn) ? 'H' : 'h'); break; 
      case 29: ch = ((g_bShiftOn) ? 'J' : 'j'); break; 
      case 30: ch = ((g_bShiftOn) ? 'K' : 'k'); break; 
      case 31: ch = ((g_bShiftOn) ? 'L' : 'l'); break; 
      case 32: ch = ((g_bShiftOn) ? ':' : ';'); break; 
      case 33: ch = '\n'; break;
      case 35: if (g_bCtrl) ch = 26; /* CTRL-Z */
			   else {if (g_bFunc) ch = '\\'; else ch = ((g_bShiftOn) ? 'Z' : 'z');}
			   break; 
      case 36: if (g_bFunc) ch = 131; /* down arrow */ else ch = ((g_bShiftOn) ? 'X' : 'x'); break; 
      case 37: if (g_bCtrl) ch = 3; /* CTRL-C */
			   else { if (g_bFunc) ch = '`'; else ch = ((g_bShiftOn) ? 'C' : 'c');} 
			   break; 
      case 38: ch = ((g_bShiftOn) ? 'V' : 'v'); break; 
      case 39: ch = ((g_bShiftOn) ? 'B' : 'b'); break; 
      case 40: ch = ((g_bShiftOn) ? 'N' : 'n'); break; 
      case 41: ch = ((g_bShiftOn) ? 'M' : 'm'); break; 
      case 42: ch = ((g_bShiftOn) ? '<' : ','); break; 
      case 43: ch = ((g_bShiftOn) ? '>' : '.'); break; 
      case 44: break;
	   case 45: break;
	   case 46: break;
	   case 47: ch = ' '; break;
	   case 48: break;

      default: break;
   }

   return ch;
}

/* Scans keyboard.
 * Returns 0 if no key pressed.
 * Returns row# (1-8) if key pressed.
 * Sets global flags indicating special/control key pressed.
 * Column is calculated from the read key code.
 * This function is called from timer 0 interrupt handler.
 */
BYTE ScanKb(void)
{
	BYTE ret=0;
	BYTE row=0;
   BYTE col=0;
   BYTE i=0;
	BYTE j=0;
	BYTE keyscan=0;
	BYTE kcode=0;

	if (g_bKeyHit) return 0;	// key is already waiting
   g_ucKeyCode = 0;
   g_cKey = 0;
   ResetSpecKeysFlags();
	for (row=0, i=1; i != 0; i <<= 1, row++)
	{
	   KBP2 = 0xFF;     // port P2 as input
      // ~i is a LOW (0) state going right to left
      // output the ~i to KBP1 
	   KBP1 = ~i; // set row #row to LOW (0) on KBP1
	   keyscan = KBP2;  // read port KBP2 (columns)
      // if the key was pressed during scan, keyscan should have
      // a LO bit set at the row and column cross, where the
      // contact is
		KBP1 = 0xFF;	  // turn off KBP1 output
	   if (keyscan != 0xFF) // we've got a read, look in which column
	   { 
		  // look for column of pressed key indicated by state 0 on KBP2 input
		  for(j=1, col=0; j != 0; j <<= 1, col++)
		  {
		    if (~keyscan & j)
			 {
				kcode = g_ucaKbMatrix[row][col];
				switch (kcode) {
               case 0:  break;
					case 44: g_bShiftOn = TRUE; break;
					case 45: g_bShiftOn = TRUE; g_bLock = TRUE; break;
					case 46: g_bCtrl = TRUE; break;
					case 48: g_bFunc = TRUE; break;
					default:
								g_ucRow = row;
								g_ucColumn = col;
								ret = row+1;
								g_ucKeyCode = kcode;
								g_bKeyHit = TRUE;
								break;
				}
			 }
		  }
	   }
	}
   if (g_bKeyHit && g_ucKeyCode) g_cKey = convKeyCode2Char(g_ucKeyCode);

	return ret;
}

/*
 * Keyboard buffer is a ring/round-about FIFO register.
 * This function adds the key code kc to the buffer.
 */
void add2KeyBuf(BYTE kc)
{
   g_ucaKeyBuf[g_ucKeyBufEndIndex] = kc;
	g_ucKeyBufEndIndex++;
	if (g_ucKeyBufEndIndex >= KEYBUFLEN)
	{
		g_ucKeyBufEndIndex = 0;
	}
	g_ucaKeyBuf[g_ucKeyBufEndIndex] = 0;
}

/*
 * Get the key from buffer.
 */
BYTE getKeyFromBuf(void)
{
   BYTE kbk = 0;

	if (g_ucKeyBufStartIndex != g_ucKeyBufEndIndex)
	{
	   kbk = g_ucaKeyBuf[g_ucKeyBufStartIndex];
		g_ucaKeyBuf[g_ucKeyBufStartIndex] = 0;
		g_ucKeyBufStartIndex++;
		if (g_ucKeyBufStartIndex >= KEYBUFLEN)
		{
			g_ucKeyBufStartIndex = 0;
		}
	}

	return kbk;
}

/* 
 * Keyboard serial bus communication driver functions.
 *
 * NOTE: in/out data and clk signals inverted
 * 
 */

/* 
 * Send start condition.
 */
void KbSerial_Start(void)
{
   SDA_OUT = BUS_HIGH;
   SCL_OUT = BUS_HIGH;
   delms(SIG_DEL);
   SDA_OUT = BUS_LOW;
   SCL_OUT = BUS_LOW;
   delms(SIG_DEL);
}

/*
 * Write single bit to bus.
 */
void KbSerial_WriteBit(BOOL bitval)
{
   SDA_OUT = ((bitval) ? BUS_HIGH : BUS_LOW);
	SCL_OUT = BUS_HIGH;
   delms(SIG_DEL);
	SCL_OUT = BUS_LOW;
   delms(SIG_DEL);
}

/*
 * Write 8-bits to bus.
 */
void KbSerial_WriteByte(BOOL sendstart,
						      BYTE byteval)
{
   BYTE bitc = 0;

	if (sendstart)
	{
	   KbSerial_Start();
	}

	for (bitc=0; bitc<8; bitc++)
	{
	   KbSerial_WriteBit((byteval & 0x80) != 0);
		byteval <<= 1;
	}
}

/*
 * Send character to the bus.
 */
void KbSerial_SendKey(BYTE kc)
{
	SDA_OUT = BUS_HIGH;
	SCL_OUT = BUS_HIGH;
   delms(SIG_DEL);
	KbSerial_WriteByte(TRUE,kc);
}

/*
* GetScannedKey()
* Read key from key buffer.
*/
void GetScannedKey()
{
   BYTE ch = 0;
 
   if (g_bKeyHit) {
      if (g_cKey) {add2KeyBuf(g_cKey); delms(KEYRD_DEL);}
      g_bKeyHit = FALSE;
   }
}
 
/*
BYTE code szHello[7] = {
   'H', 'e', 'l', 'l', 'o', '!', 0
};
*/
 
/*
* -------------- MAIN LOOP ---------------------
*/
main()
{
   BYTE key = 0;
   //const BYTE *s = szHello;
 
   init_vars();
   init_ports();
   init_SFR();
   //delms(20);
   delms(1000);
   KbSerial_SendKey(0);
   /*
   while (*s) {
      add2KeyBuf(*s++);
   }*/
   while(TRUE)
   {
      ScanKb();
      GetScannedKey();
      while ((key = getKeyFromBuf()) != 0)
      {
         KbSerial_SendKey(key);
      }
   }
}

