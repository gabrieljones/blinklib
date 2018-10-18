/*

    This library provides the operating system for the Move38 Blinks platform.
    More info at http://Move38.com

    This sits on top of the hardware abstraction layer and handles functions like

       *Startup and loading new games
       *Sleeping
       *Time keeping

    ...basically all things that a game can not do.

*/

#include <avr/pgmspace.h>
#include <stddef.h>     // NULL

#include <util/crc16.h>     // For IR checksums 
#include <util/atomic.h>         // ATOMIC_BLOCK

#include "blinkos.h"
#include "blinkos_button.h"
#include "blinkos_timer.h"          // Class Timer 

#include "callbacks.h"              // From blinkcore, which will call into us via these

// TODO: Put this at a known fixed address to save registers
// Will require a new .section in the linker file. Argh. 

loopstate_in_t loopstate_in;
loopstate_out_t loopstate_out;

#include "pixel.h"
#include "timer.h"
#include "utils.h"
#include "power.h"
#include "button.h"

#include "ir.h"
#include "blinkos_irdata.h"


#define SLEEP_TIMEOUT_SECONDS (10*60)          // If no button press in this long then goto sleep

#define SLEEP_TIMEOUT_MS ( (millis_t) SLEEP_TIMEOUT_SECONDS  * MILLIS_PER_SECOND) // Must cast up because otherwise will overflow

// When we should fall sleep from inactivity
Timer sleepTimer;

// Turn off everything and goto to sleep

static void sleep(void) {

    pixel_disable();        // Turn off pixels so battery drain
    ir_disable();           // TODO: Wake on pixel
    button_ISR_on();        // Enable the button interrupt so it can wake us

    power_sleep();          // Go into low power sleep. Only a button change interrupt can wake us

    button_ISR_off();       // Set everything back to thew way it was before we slept
    ir_enable();
    pixel_enable();

    loopstate_in.woke_flag= 1;

}


void postponeSleep() {
    sleepTimer.set( SLEEP_TIMEOUT_MS );    
}    

buttonstate_t buttonstate;

void timer_1000us_callback_sei(void) {

    incrementMillis1ms();
    
    if (updateButtonState1ms( buttonstate )) {        
        postponeSleep();
    }        

}

// Atomically copy button state from source to destination, then clear flags in source.

void grabAndClearButtonState(  buttonstate_t &d ) {
    
    ATOMIC_BLOCK( ATOMIC_FORCEON ) {
        
        d.bitflags = buttonstate.bitflags;
        d.clickcount = buttonstate.clickcount;
        d.down = buttonstate.down;
        buttonstate.bitflags =0;                      // Clear the flags we just grabbed (this is a one shot deal)
        
    }
    
}

// Below are the callbacks we provide to blinkcore

// This is called by timer ISR about every 512us with interrupts on.

void timer_256us_callback_sei(void) {

    // Decrementing slightly more efficient because we save a compare.
    static unsigned step_us=0;

    step_us += 256;                     // 256us between calls

    if ( step_us>= 1000 ) {             // 1000us in a 1ms
        timer_1000us_callback_sei();
        step_us-=1000;
    }
}

// This is called by timer ISR about every 256us with interrupts on.

void timer_128us_callback_sei(void) {
    IrDataPeriodicUpdateComs();
}


uint8_t  crccheck(uint8_t const * data, uint8_t len )
{
    uint8_t c = 0xFF;                          // Start CRC at 0xFF
             
    while (--len) {           // Count 1 less than total length
                 
        c=_crc8_ccitt_update( c , *data );
                 
        data++;
                 
    }
             
    // d now points to final byte, which should be CRC
             
    return *data == c;
        
}

void processPendingIRPackets() {

    // First process any IR data that came in
        
    for( uint8_t f=0; f< IR_FACE_COUNT; f++ ) {
            
        if (irDataIsPacketReady(f)) {
                
            uint8_t packetLen = irDataPacketLen(f);
                
            if ( packetLen < 2 ) {
                    
                // Packet to short to even consider
                    
                // TODO: Error counting?
                    
                irDataMarkPacketRead( f );
                    
            } else {
                    
                // IR data packet received and at least 2 bytes long
                    
                uint8_t const *data = irDataPacketBuffer(f);
                    
                // TODO: Maybe have super simple inversion check for single byte commands?
                    
                // TODO: Maybe have an extra "out-of-band" header bit so we can let userland have simple 1 byte packets?
                    
                if (crccheck(data,packetLen)) {
                        
                    // Got a good ir data packet!
                        
                    // route based on header byte
                        
                    if (*data==0x01) {  
                        // Userland data
                        
                        loopstate_in.ir_data_buffers[f].len=  packetLen-1;
                        loopstate_in.ir_data_buffers[f].ready_flag = 1;
                        
                        // TODO: Where do we mark these as read? User responsibility or ours?
                        
                        
                    } else if (*data==0x02) {
                        
                        // blinkOS packet
                        irDataMarkPacketRead(f);
                        
                    } else {
                        
                        // Thats's unexpected.
                        // Lets at least consume it so a new packet can come in
                        
                        irDataMarkPacketRead(f); 
                        
                    }                        
                        
                        
                } else {
                        
                    // Packet failed CRC check
                    // TODO: Error counting?
                        
                    irDataMarkPacketRead( f );
                        
                } // if (crccheck(data,packetLen))
                    
            }
        }  // irIsdataPacketReady(f)
    }    // for( uint8_t f=0; f< IR_FACE_COUNT; f++ )

}    


// This is the entry point where the blinkcore platform will pass control to
// us after initial power-up is complete

// We make this weak so that a game can override and take over before we initialize all the higher level stuff

extern void setupEntry();

void run(void) {
    
    // Set the buffer pointers 
    
    for( uint8_t f=0; f< IR_FACE_COUNT; f++ ) {
        
        loopstate_in.ir_data_buffers[f].data = irDataPacketBuffer( f ) +1 ;     // We use 1st byte for routing, so we give userland everything after that 
        loopstate_in.ir_data_buffers[f].ready_flag = 0; 
        
    }        
    
    ir_enable();

    pixel_enable();

    button_enable_pu();
    
    // Call user setup code
           
    setupEntry();
    
    postponeSleep();            // We just turned on, so restart sleep timer

    while (1) {
        
        updateMillisSnapshot();             // Use the snapshot do we don't have to turn off interrupts
                                            // every time we want to check this multibyte value
        
        
        // Let's get loopstate_in all set up for the call into the user process
        
        processPendingIRPackets();          // Sets the irdatabuffers in loopstate_in. Also processes any received IR blinkOS commands
                                            // I wish this didn't directly access the loopstate_in buffers, but the abstraction would 
                                            // cost lots of unnecessary coping
        
        grabAndClearButtonState( loopstate_in.buttonstate );     // Make a local copy of the instant button state to pass to userland. Also clears the flags for next time. 
        
        loopstate_in.millis = millis_snapshot;

        loopEntry( &loopstate_in , &loopstate_out );
        
        for( uint8_t f = 0; f < PIXEL_FACE_COUNT ; f++ ) {
            
            pixelColor_t color = loopstate_out.colors[f];
            
            if (  color.reserved ) {          // Did the color change on the last pass? (We used the reserved bit to tell in the blnkOS API)
                                
                pixel_bufferedSetPixel( f,  color  );           // TODO: Do we need to clear that top bit? 
            }                
            
        }            

        pixel_displayBufferedPixels();      // show all display updates that happened in last loop()
                                            // Also currently blocks until new frame actually starts

        if (sleepTimer.isExpired()) {
            sleep();

        }


        // TODO: Possible sleep util next timer tick?
    }

}
