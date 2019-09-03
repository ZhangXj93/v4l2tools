/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** v4l2compress_x264.cpp
** 
** Read YUYV from a V4L2 capture -> compress in H264 -> write to a V4L2 output device
**
** -------------------------------------------------------------------------*/

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <signal.h>

#include <fstream>

extern "C" 
{
	#include "x264.h"
}

#include "libyuv.h"

#include "logger.h"

#include "V4l2Device.h"
#include "V4l2Capture.h"
#include "V4l2Output.h"

bool stop=false;
void sighandler(int)
{ 
       printf("SIGINT\n");
       stop = true;
}

/* ---------------------------------------------------------------------------
**  main
** -------------------------------------------------------------------------*/
int main(int argc, char* argv[]) 
{	
	int verbose=0;
	const char *in_devname = "/dev/video0";	
	const char *out_devname = "/dev/video1";
	int fps = 25;

	int c = 0;
	V4l2Access::IoType ioTypeIn  = V4l2Access::IOTYPE_MMAP;
	V4l2Access::IoType ioTypeOut = V4l2Access::IOTYPE_MMAP;
	int rc_method = -1;
	int rc_value = 0;
	std::string inFormatStr = "YUYV";
	
	while ((c = getopt (argc, argv, "hv::rwF:" "q:f:")) != -1)
	{
		switch (c)
		{
			case 'v':	verbose = 1; if (optarg && *optarg=='v') { verbose++; };  break;
			
			case 'F':       fps = atoi(optarg); break;			

			case 'r':	ioTypeIn  = V4l2Access::IOTYPE_READWRITE; break;			
			case 'w':	ioTypeOut = V4l2Access::IOTYPE_READWRITE; break;	

			case 'q':	rc_method = X264_RC_CQP; rc_value = atoi(optarg); break;	
			case 'f':	rc_method = X264_RC_CRF;  rc_value = atof(optarg); break;	
			
			case 'h':
			{
				std::cout << argv[0] << " [-v[v]] [-W width] [-H height] source_device dest_device" << std::endl;
				std::cout << "\t -v            : verbose " << std::endl;
				std::cout << "\t -vv           : very verbose " << std::endl;
							
				std::cout << "\t -r            : V4L2 capture using read interface (default use memory mapped buffers)" << std::endl;
				std::cout << "\t -w            : V4L2 capture using write interface (default use memory mapped buffers)" << std::endl;				
				
				
				std::cout << "\t source_device : V4L2 capture device (default "<< in_devname << ")" << std::endl;
				std::cout << "\t dest_device   : V4L2 capture device (default "<< out_devname << ")" << std::endl;
				exit(0);
			}
		}
	}
	if (optind<argc)
	{
		in_devname = argv[optind];
		optind++;
	}	
	if (optind<argc)
	{
		out_devname = argv[optind];
		optind++;
	}	
		
	// initialize log4cpp
	initLogger(verbose);

	// init V4L2 capture interface
	V4L2DeviceParameters param(in_devname,0,0,0,0,verbose);
	V4l2Capture* videoCapture = V4l2Capture::create(param, ioTypeIn);
	
	if (videoCapture == NULL)
	{	
		LOG(WARN) << "Cannot create V4L2 capture interface for device:" << in_devname; 
	}
	else
	{
		// init V4L2 output interface
		int width = videoCapture->getWidth();
		int height = videoCapture->getHeight();
		V4L2DeviceParameters outparam(out_devname, V4L2_PIX_FMT_H264, videoCapture->getWidth(), videoCapture->getHeight(), 0, verbose);
		V4l2Output* videoOutput = V4l2Output::create(outparam, ioTypeOut);
		if (videoOutput == NULL)
		{	
			LOG(WARN) << "Cannot create V4L2 output interface for device:" << out_devname; 
		}
		else
		{		
			LOG(NOTICE) << "Start Capturing from " << in_devname; 
			x264_param_t param;
			x264_param_default_preset(&param, "ultrafast", "zerolatency");
			if (verbose>1)
			{
				param.i_log_level = X264_LOG_DEBUG;
			}
			param.i_threads = 1;
			param.i_width = width;
			param.i_height = height;
			param.i_fps_num = fps;
			param.i_fps_den = 1;
			param.i_keyint_min = fps;
			param.i_keyint_max = fps;
			param.i_bframe = 0;
			param.b_repeat_headers = 1;
			
			if (rc_method == X264_RC_CQP) {
				param.rc.i_rc_method = rc_method;
				param.rc.i_qp_constant = rc_value;
				param.rc.i_qp_min = rc_value; 
				param.rc.i_qp_max = rc_value;
			} else if (rc_method == X264_RC_CRF) {
				param.rc.i_rc_method = rc_method;
				param.rc.f_rf_constant = rc_value;
				param.rc.f_rf_constant_max = rc_value;
			}
			LOG(WARN) << "rc_method:" << param.rc.i_rc_method; 
			LOG(WARN) << "i_qp_constant:" << param.rc.i_qp_constant; 
			LOG(WARN) << "f_rf_constant:" << param.rc.f_rf_constant; 
			
			
			x264_t* encoder = x264_encoder_open(&param);
			if (!encoder)
			{
				LOG(WARN) << "Cannot create X264 encoder for device:" << in_devname; 
			}
			else
			{		
				x264_picture_t pic_in;
				x264_picture_init( &pic_in );
				x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);
				
				x264_picture_t pic_out;
				
				timeval tv;
				timeval refTime;
				timeval curTime;
				
				LOG(NOTICE) << "Start Compressing " << in_devname << " to " << out_devname; 					
				signal(SIGINT,sighandler);
				while (!stop) 
				{
					tv.tv_sec=1;
					tv.tv_usec=0;
					int ret = videoCapture->isReadable(&tv);
					if (ret == 1)
					{
						gettimeofday(&refTime, NULL);	
						char buffer[videoCapture->getBufferSize()];
						int rsize = videoCapture->read(buffer, sizeof(buffer));
						
						gettimeofday(&curTime, NULL);												
						timeval captureTime;
						timersub(&curTime,&refTime,&captureTime);
						refTime = curTime;
						
						ConvertToI420((const uint8*)buffer, rsize,
							pic_in.img.plane[0], width,
							pic_in.img.plane[1], width/2,
							pic_in.img.plane[2], width/2,
							0, 0,
							width, height,
							width, height,
							libyuv::kRotate0, videoCapture->getFormat());

						gettimeofday(&curTime, NULL);												
						timeval convertTime;
						timersub(&curTime,&refTime,&convertTime);
						refTime = curTime;
						
						x264_nal_t* nals = NULL;
						int i_nals = 0;
						x264_encoder_encode(encoder, &nals, &i_nals, &pic_in, &pic_out);
						
						gettimeofday(&curTime, NULL);												
						timeval endodeTime;
						timersub(&curTime,&refTime,&endodeTime);
						refTime = curTime;
						
						if (i_nals > 1) {
							int size = 0;
							for (int i=0; i < i_nals; ++i) {
								size+=nals[i].i_payload;
							}
							char buffer[size];
							char* ptr = buffer;
							for (int i=0; i < i_nals; ++i) {
								memcpy(ptr, nals[i].p_payload, nals[i].i_payload);									
								ptr+=nals[i].i_payload;
							}
							
							int wsize = videoOutput->write(buffer,size);
							LOG(INFO) << "Copied nbnal:" << i_nals << " size:" << wsize; 					
							
						} else if (i_nals == 1) {
							int wsize = videoOutput->write((char*)nals[0].p_payload, nals[0].i_payload);
							LOG(INFO) << "Copied size:" << wsize; 					
						}
						
						gettimeofday(&curTime, NULL);												
						timeval writeTime;
						timersub(&curTime,&refTime,&writeTime);
						refTime = curTime;

						LOG(DEBUG) << "dts:" << pic_out.i_dts << " captureTime:" << (captureTime.tv_sec*1000+captureTime.tv_usec/1000) 
								<< " convertTime:" << (convertTime.tv_sec*1000+convertTime.tv_usec/1000)					
								<< " endodeTime:" << (endodeTime.tv_sec*1000+endodeTime.tv_usec/1000)
								<< " writeTime:" << (writeTime.tv_sec*1000+writeTime.tv_usec/1000); 					
						
					}
					else if (ret == -1)
					{
						LOG(NOTICE) << "stop error:" << strerror(errno); 
						stop=true;
					}
				}
				
				x264_picture_clean(&pic_in);
				x264_encoder_close(encoder);
			}
			delete videoOutput;			
		}
		delete videoCapture;
	}
	
	return 0;
}
