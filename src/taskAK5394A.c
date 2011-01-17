/*
 * taskAK5394A.c
 *
 *  Created on: Feb 14, 2010
 *      Author: Alex
 *
 * Modified by Alex Lee and SDR-Widget team for the sdr-widget project.
 * See http://code.google.com/p/sdr-widget/
 * Copyright under GNU General Public License v2
 */


//_____  I N C L U D E S ___________________________________________________

//#include <stdio.h>
#include "usart.h"     // Shall be included before FreeRTOS header files, since 'inline' is defined to ''; leading to
                       // link errors
#include "conf_usb.h"


#include <avr32/io.h>
#if __GNUC__
#  include "intc.h"
#endif
#include "board.h"
#ifdef FREERTOS_USED
#include "FreeRTOS.h"
#include "task.h"
#endif
#include "usb_drv.h"
#include "gpio.h"
#if LCD_DISPLAY				// Multi-line LCD display
#include "taskLCD.h"
#endif
//#include "taskEXERCISE.h"
#include "ssc_i2s.h"
#include "pm.h"
#include "pdca.h"
#include "usb_standard_request.h"
#include "usb_specific_request.h"
#include "taskAK5394A.h"

//_____ M A C R O S ________________________________________________________


//_____ D E F I N I T I O N S ______________________________________________

//_____ D E C L A R A T I O N S ____________________________________________

static const gpio_map_t SSC_GPIO_MAP =
{
		{SSC_RX_CLOCK, SSC_RX_CLOCK_FUNCTION},
		{SSC_RX_DATA, SSC_RX_DATA_FUNCTION},
		{SSC_RX_FRAME_SYNC, SSC_RX_FRAME_SYNC_FUNCTION},
		{SSC_TX_CLOCK, SSC_TX_CLOCK_FUNCTION},
		{SSC_TX_DATA, SSC_TX_DATA_FUNCTION},
		{SSC_TX_FRAME_SYNC, SSC_TX_FRAME_SYNC_FUNCTION}
};

static const pdca_channel_options_t PDCA_OPTIONS =
	  {
	    .addr = (void *)audio_buffer_0,         // memory address
	    .pid = AVR32_PDCA_PID_SSC_RX,           // select peripheral
	    .size = AUDIO_BUFFER_SIZE,              // transfer counter
	    .r_addr = NULL,                         // next memory address
	    .r_size = 0,                            // next transfer counter
	    .transfer_size = PDCA_TRANSFER_SIZE_WORD  // select size of the transfer - 32 bits
	  };

volatile U32 audio_buffer_0[AUDIO_BUFFER_SIZE];
volatile U32 audio_buffer_1[AUDIO_BUFFER_SIZE];

volatile avr32_ssc_t *ssc = &AVR32_SSC;

volatile int audio_buffer_in;

void AK5394A_task(void*);


/*! \brief The PDCA interrupt handler.
 *
 * The handler reload the PDCA settings with the correct address and size using the reload register.
 * The interrupt will happen when the reload counter reaches 0
 */
__attribute__((__interrupt__)) static void pdca_int_handler(void)
{

  if (audio_buffer_in == 0)
  {
    // Set PDCA channel reload values with address where data to load are stored, and size of the data block to load.
   pdca_reload_channel(PDCA_CHANNEL_SSC_RX, (void *)audio_buffer_1, AUDIO_BUFFER_SIZE);
   audio_buffer_in = 1;
  }
  else // if (audio_buffer_in == 1)
  {
    pdca_reload_channel(PDCA_CHANNEL_SSC_RX, (void *)audio_buffer_0, AUDIO_BUFFER_SIZE);
    audio_buffer_in = 0;
  }
}


/*! \brief Init interrupt controller and register pdca_int_handler interrupt.
 */
void pdca_set_irq(void)
{
  // Disable all interrupt/exception.
  Disable_global_interrupt();

  // Register the compare interrupt handler to the interrupt controller
  // and enable the compare interrupt.
  // (__int_handler) &pdca_int_handler The handler function to register.
  // AVR32_PDCA_IRQ_0 The interrupt line to register to.
  // AVR32_INTC_INT2  The priority level to set for this interrupt line.  INT0 is lowest.
  // INTC_register_interrupt(__int_handler handler, int line, int priority);
  INTC_register_interrupt( (__int_handler) &pdca_int_handler, AVR32_PDCA_IRQ_0, AVR32_INTC_INT2);

  // Enable all interrupt/exception.
  Enable_global_interrupt();
}





//!
//! @brief This function initializes the hardware/software resources
//! required for device CDC task.
//!
void AK5394A_task_init(void)
{

	// Set up AK5394A
	gpio_clr_gpio_pin(AK5394_RSTN);		// put AK5394A in reset
	gpio_set_gpio_pin(AK5394_DFS0);		// L H  -> 96khz
	gpio_clr_gpio_pin(AK5394_DFS1);
	gpio_set_gpio_pin(AK5394_HPFE);		// enable HP filter
	gpio_clr_gpio_pin(AK5394_ZCAL);		// use VCOML and VCOMR to cal
	gpio_set_gpio_pin(AK5394_SMODE1);	// SMODE1 = H for Master i2s
	gpio_set_gpio_pin(AK5394_SMODE2);	// SMODE2 = H for Master/Slave i2s

    gpio_set_gpio_pin(AK5394_RSTN);		// start AK5394A
    while (gpio_get_pin_value(AK5394_CAL)); // wait till CAL goes low

		// Assign GPIO to SSC.
	  gpio_enable_module(SSC_GPIO_MAP, sizeof(SSC_GPIO_MAP) / sizeof(SSC_GPIO_MAP[0]));
	  gpio_enable_pin_glitch_filter(SSC_RX_CLOCK);
	  gpio_enable_pin_glitch_filter(SSC_RX_DATA);
	  gpio_enable_pin_glitch_filter(SSC_RX_FRAME_SYNC);

	  current_freq.frequency = 96000;

	  // set up SSC
	  ssc_i2s_init(ssc, 96000, 24, 32, SSC_I2S_MODE_SLAVE_STEREO_IN, FPBA_HZ);

	  // set up PDCA
	  // In order to avoid long slave handling during undefined length bursts (INCR), the Bus Matrix
	  // provides specific logic in order to re-arbitrate before the end of the INCR transfer.
	  //
	  // HSB Bus Matrix: By default the HSB bus matrix mode is in Undefined length burst type (INCR).
	  // Here we have to put in single access (the undefined length burst is treated as a succession of single
	  // accesses, allowing re-arbitration at each beat of the INCR burst.
	  // Refer to the HSB bus matrix section of the datasheet for more details.
	  //
	  // HSB Bus matrix register MCFG1 is associated with the CPU instruction master interface.
	  AVR32_HMATRIX.mcfg[AVR32_HMATRIX_MASTER_CPU_INSN] = 0x1;

	  // Register PDCA IRQ interrupt.
	  pdca_set_irq();

	  // Init PDCA channel with the pdca_options.
	  pdca_init_channel(PDCA_CHANNEL_SSC_RX, &PDCA_OPTIONS); // init PDCA channel with options.
      pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);


	  xTaskCreate(AK5394A_task,
              configTSK_AK5394A_NAME,
              configTSK_AK5394A_STACK_SIZE,
              NULL,
              configTSK_AK5394A_PRIORITY,
              NULL);

}


//!
//! @brief Entry point of the AK5394A task management
//!
void AK5394A_task(void *pvParameters)
{
  portTickType xLastWakeTime;
  xLastWakeTime = xTaskGetTickCount();

  while (TRUE)
  {
	  // All the hardwork is done by the pdca and the interrupt handler.
	  // Just check whether sampling freq is changed, to do rate change etc.

    vTaskDelayUntil(&xLastWakeTime, configTSK_AK5394A_PERIOD);


    if (freq_changed){


    	if (current_freq.frequency == 96000){
	   	pdca_disable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
	    pdca_disable(PDCA_CHANNEL_SSC_RX);

    		gpio_set_gpio_pin(AK5394_DFS0);		// L H  -> 96khz
    		gpio_clr_gpio_pin(AK5394_DFS1);

    	}
    	else if (current_freq.frequency == 192000)
    	{
    	pdca_disable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
        pdca_disable(PDCA_CHANNEL_SSC_RX);

				gpio_clr_gpio_pin(AK5394_DFS0);		// H L -> 192khz
        		gpio_set_gpio_pin(AK5394_DFS1);

    	}
    	else if (current_freq.frequency == 48000) // 48khz
    	{

        	pdca_disable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
            pdca_disable(PDCA_CHANNEL_SSC_RX);

				gpio_clr_gpio_pin(AK5394_DFS0);		// L L  -> 48khz
        		gpio_clr_gpio_pin(AK5394_DFS1);

        }

    	// re-sync SSC to LRCK
        // Wait for the next frame synchronization event
        // to avoid channel inversion.  Start with left channel - FS goes low
        while (!gpio_get_pin_value(AK5394_LRCK));
        while (gpio_get_pin_value(AK5394_LRCK));

        // Enable now the transfer.
        pdca_enable(PDCA_CHANNEL_SSC_RX);

        // Init PDCA channel with the pdca_options.
        pdca_init_channel(PDCA_CHANNEL_SSC_RX, &PDCA_OPTIONS); // init PDCA channel with options.
        pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);

        // reset freq_changed flag
        freq_changed = FALSE;
    }

  }
}


