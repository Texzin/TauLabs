/**
 ******************************************************************************
 *
 * @file       vibrationtest.c
 * @author     Tau Labs, http://www.taulabs.org Copyright (C) 2013.
 * @brief      VibrationTest module to be used as a template for actual modules.
 *             Event callback version.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: ExampleObject1, ExampleSettings
 * Output object: ExampleObject2
 *
 * This module executes in response to ExampleObject1 updates. When the
 * module is triggered it will update the data of ExampleObject2.
 *
 * No threads are used in this example.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "openpilot.h"
#include "arm_math.h"

#include "accels.h"
#include "accessorydesired.h"
#include "modulesettings.h"
#include "vibrationtestoutput.h"
#include "vibrationtestsettings.h"


// Private constants

#define STACK_SIZE_BYTES (200 + 460 + (26*fft_window_size)) //This value has been calculated to leave 200 bytes of stack space, no matter the fft_window_size
#define TASK_PRIORITY (tskIDLE_PRIORITY+1)

// Private variables
static xTaskHandle taskHandle;
static bool module_enabled = false;
static uint16_t fft_window_size;

static struct VibrationTest_data {
	bool access_accels;
	uint16_t accels_sum_count;
	float accels_data_sum_x;
	float accels_data_sum_y;
	float accels_data_sum_z;
	
	float accels_static_bias_x; // In all likelyhood, the initial values will be close to 
	float accels_static_bias_y; // (0,0,g). In the case where they are not, this will still  
	float accels_static_bias_z; // converge to the true bias in a few thousand measurements.	
} *vtd;


// Private functions
static void VibrationTestTask(void *parameters);
static void accelsUpdatedCb(UAVObjEvent * objEv);

/**
 * Start the module, called on startup
 */
static int32_t VibrationTestStart(void)
{
	
	if (!module_enabled)
		return -1;

	//Add callback for averaging accelerometer data
	AccelsConnectCallback(&accelsUpdatedCb);
	
	// Allocate and initialize the static data storage only if module is enabled
	vtd = (struct VibrationTest_data *) pvPortMalloc(sizeof(struct VibrationTest_data));
	if (vtd == NULL) {
		module_enabled = false;
		return -1;
	}
	
	// make sure that all inputs[] are zeroed...
	memset(vtd, 0, sizeof(struct VibrationTest_data));
	//... except for Z axis static bias
	vtd->accels_static_bias_z=9.81; // [See note in definition of VibrationTest_data structure]
	
	// Start main task
	xTaskCreate(VibrationTestTask, (signed char *)"VibrationTest", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &taskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_VIBRATIONTEST, taskHandle);
	return 0;
}

/**
 * Initialise the module, called on startup
 */

static int32_t VibrationTestInitialize(void)
{
	ModuleSettingsInitialize();
	
#ifdef MODULE_VibrationTest_BUILTIN
	module_enabled = true;
#else
	uint8_t module_state[MODULESETTINGS_STATE_NUMELEM];
	ModuleSettingsStateGet(module_state);
	if (module_state[MODULESETTINGS_STATE_VIBRATIONTEST] == MODULESETTINGS_STATE_ENABLED) {
		module_enabled = true;
	} else {
		module_enabled = false;
	}
#endif
	
	if (!module_enabled) //If module not enabled...
		return -1;

	// Initialize UAVOs
	VibrationTestSettingsInitialize();
	VibrationTestOutputInitialize();
	
	//Get the FFT window size
	VibrationTestSettingsFFTWindowSizeOptions fft_window_size_enum;
	VibrationTestSettingsFFTWindowSizeGet(&fft_window_size_enum);
	switch (fft_window_size_enum) {
		case VIBRATIONTESTSETTINGS_FFTWINDOWSIZE_16:
			fft_window_size = 16;
			break;
		case VIBRATIONTESTSETTINGS_FFTWINDOWSIZE_64:
			fft_window_size = 64;
			break;
		case VIBRATIONTESTSETTINGS_FFTWINDOWSIZE_256:
			fft_window_size = 256;
			break;
		case VIBRATIONTESTSETTINGS_FFTWINDOWSIZE_1024:
			fft_window_size = 1024;
			break;
		default:
			//This represents a serious configuration error. Do not start module.
			module_enabled = false;
			return -1;
			break;
	}
		
	return 0;
	
}
MODULE_INITCALL(VibrationTestInitialize, VibrationTestStart)

static void VibrationTestTask(void *parameters)
{
#define MAX_BLOCKSIZE   2048
	
	portTickType lastSysTime;
	uint8_t sample_count;
		
	//Create the buffers
	float accel_buffer_complex_x[fft_window_size*2]; //These buffers are complex numbers, so they are twice
	float accel_buffer_complex_y[fft_window_size*2]; // as long as the number of samples, and  complex part 
	float accel_buffer_complex_z[fft_window_size*2]; // is always 0.
	
/** These values are useful for insight into the Fourier transform performed by this module.
	float freq_sample = 1.0f/(sampleRate_ms / portTICK_RATE_MS);
	float freq_nyquist = f_s/2.0f;
	uint16_t num_samples = fft_window_size;
 */

	// Create histogram bin instances for vibration test. Start from i=1 because the first instance is 
	// generated by VibrationTestOutputInitialize(). Generate three times the length because there are 
	// three vectors. Generate half the length because the FFT output is symmetric about the mid-frequency, 
	// so there's no point in using memory additional memory.
	for (int i=1; i<(fft_window_size>>1); i++) {
		VibrationTestOutputCreateInstance();
	}
	
	// Main task loop
	VibrationTestOutputData vibrationTestOutputData;
	sample_count = 0;
	lastSysTime = xTaskGetTickCount();
	while(1)
	{
		uint16_t sampleRate_ms;
		VibrationTestSettingsSampleRateGet(&sampleRate_ms);
		sampleRate_ms = sampleRate_ms > 0 ? sampleRate_ms : 1; //Ensure sampleRate never is 0.
		
		vTaskDelayUntil(&lastSysTime, sampleRate_ms / portTICK_RATE_MS);

		//Only read the samples if there are new ones
		if(vtd->accels_sum_count){
			vtd->access_accels=true; //This keeps the callback from altering the accelerometer sums

			//Calculate averaged values
			float accels_avg_x=vtd->accels_data_sum_x/vtd->accels_sum_count;
			float accels_avg_y=vtd->accels_data_sum_y/vtd->accels_sum_count;
			float accels_avg_z=vtd->accels_data_sum_z/vtd->accels_sum_count;
			
			//Calculate DC bias
			float alpha=.01; //Hard coded drift very slowly
			vtd->accels_static_bias_x=alpha*accels_avg_x + (1-alpha)*vtd->accels_static_bias_x;
			vtd->accels_static_bias_y=alpha*accels_avg_y + (1-alpha)*vtd->accels_static_bias_y;
			vtd->accels_static_bias_z=alpha*accels_avg_z + (1-alpha)*vtd->accels_static_bias_z;
			
			// Add averaged values to the buffer, and remove DC bias
			accel_buffer_complex_x[sample_count*2]=accels_avg_x - vtd->accels_static_bias_x;
			accel_buffer_complex_y[sample_count*2]=accels_avg_y - vtd->accels_static_bias_y;
			accel_buffer_complex_z[sample_count*2]=accels_avg_z - vtd->accels_static_bias_z;
				
			//Reset the accumulators
			vtd->accels_data_sum_x=0;
			vtd->accels_data_sum_y=0;
			vtd->accels_data_sum_z=0;
			vtd->accels_sum_count=0;
				
			vtd->access_accels=false; //Return control to the callback
			}
		else {
			//If there are no new samples, go back to the beginning
			continue;
		}
		
		//Set complex part to 0
		accel_buffer_complex_x[sample_count*2+1]=0;
		accel_buffer_complex_y[sample_count*2+1]=0;
		accel_buffer_complex_z[sample_count*2+1]=0;

		//Advance sample and reset when at buffer end
		sample_count++;
		if (sample_count >= fft_window_size) {
			sample_count=0;
		}
		
		//Only process once the buffers are filled. This could be done continuously, but this way is probably easier on the processor
		if (sample_count==0) {
			// Decalare variables
			float fft_output[fft_window_size>>1]; //Output is symmetric, so no need to store second half of output
			arm_cfft_radix4_instance_f32 cfft_instance;
			arm_status status;
			
			// Initialize the CFFT/CIFFT module
			status = ARM_MATH_SUCCESS;
			bool ifftFlag = false;
			bool doBitReverse = 1;
			status = arm_cfft_radix4_init_f32(&cfft_instance, fft_window_size, ifftFlag, doBitReverse);
			
			// Perform the DFT on each of the three axes
			for (int i=0; i < 3; i++) {
				if (status == ARM_MATH_SUCCESS) {
					
					//Create pointer and assign buffer vectors to it
					float *ptrCmplxVec;
					
					switch (i) {
						case 0:
							ptrCmplxVec=accel_buffer_complex_x;
							break;
						case 1:
							ptrCmplxVec=accel_buffer_complex_y;
							break;
						case 2:
							ptrCmplxVec=accel_buffer_complex_z;
							break;
						default:
							//Whoops, this is a major error, leave before we overwrite memory
							continue;
					}
					
					// Process the data through the CFFT/CIFFT module. This is an in-place
					// operation, so the FFT output is saved onto the input buffer. Moving
					// forward from this point, ptrCmplxVec contains the DFT of the
					// acceleration signal.
					arm_cfft_radix4_f32(&cfft_instance, ptrCmplxVec);
					
					// Process the data through the Complex Magnitude Module. This calculates
					// the magnitude of each complex number, so that the output is a scalar
					// magnitude without complex phase. Only the first half of the values are
					// calculated because in a Fourier transform the second half is symmetric.
					arm_cmplx_mag_f32(ptrCmplxVec, fft_output, fft_window_size>>1);
					memcpy(ptrCmplxVec, fft_output, (fft_window_size>>1) * sizeof(float));					
				}
			}
			
			//Write output to UAVO
			for (int j=0; j<(fft_window_size>>1); j++) 
			{
				//Assertion check that we are not trying to write to instances that don't exist
				if (j >= UAVObjGetNumInstances(VibrationTestOutputHandle()))
					continue;
				
				vibrationTestOutputData.x = accel_buffer_complex_x[j];
				vibrationTestOutputData.y = accel_buffer_complex_y[j];
				vibrationTestOutputData.z = accel_buffer_complex_z[j];
				VibrationTestOutputInstSet(j, &vibrationTestOutputData);
			}
			
			
			// Erase buffer, which has the effect of setting the complex part to 0.
			memset(accel_buffer_complex_x, 0, sizeof(accel_buffer_complex_x));
			memset(accel_buffer_complex_y, 0, sizeof(accel_buffer_complex_y));
			memset(accel_buffer_complex_z, 0, sizeof(accel_buffer_complex_z));			
		}
	}
}


/**
 * Accumulate accelerometer data. This would be a great place to add a 
 * high-pass filter, in order to eliminate the DC bias from gravity.
 * Until then, a DC bias subtraction has been added in the main loop.
 */

static void accelsUpdatedCb(UAVObjEvent * objEv) 
{
	if(!vtd->access_accels){
		AccelsData accels_data;
		AccelsGet(&accels_data);
		
		vtd->accels_data_sum_x+=accels_data.x;
		vtd->accels_data_sum_y+=accels_data.y;
		vtd->accels_data_sum_z+=accels_data.z;
		
		vtd->accels_sum_count++;
	}
}
