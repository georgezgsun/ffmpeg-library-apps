#include "ffmpeg.h"
#include <Windows.h>

using namespace ffmpeg;
using namespace std;

// global variables
CircularBuffer* cbuf; // global shared the circular buffer
Demuxer* ipCam; // global shared IP camera
//AVFormatContext* ifmt_Ctx = NULL;  // global shared input format context
int64_t lastReadPacktTime = 0; // global shared time stamp for call back
string prefix_videofile = "C:\\Users\\georges\\Documents\\CopTraxTemp\\";
int Debug = 2;

// This is the sub thread that captures the video streams from specified IP camera and saves them into the circular buffer
// void* videoCapture(void* myptr)
DWORD WINAPI videoCapture(LPVOID myPtr)
{
	AVPacket pkt;

	int ret;

	AVRational tb = ipCam->get_stream_time_base();
	tb.num *= 1000; // change the time base to be ms based
	int index_video = ipCam->get_video_index();

	// read packets from IP camera and save it into circular buffer
	while (true)
	{
		//ret = av_read_frame(ifmt_Ctx, &pkt); // read a frame from the camera
		ret = ipCam->read_packet(&pkt);

		// handle the timeout, blue screen shall be added here
		if (ret < 0)
		{
			fprintf(stderr, "%s, error code=%d.\n", ipCam->get_error_message().c_str(), ret);
			continue;
		}

		if (pkt.stream_index == index_video)
		{
			ret = cbuf->push_packet(&pkt);  // add the packet to the circular buffer
			if (ret >= 0)
			{
				if (Debug > 2)
				{
					fprintf(stderr, "Added a new packet (%lldms, %d). Poped %d packets. The circular buffer has %d packets with size %d now.\n",
						pkt.pts * tb.num / tb.den, pkt.size, ret, cbuf->get_total_packets(), cbuf->get_size());
				}
			}
			else
			{
				fprintf(stderr, "Error %s. Packet (%lldms, %d) is not added. The circular buffer has %d packets with size %d now\n",
					cbuf->get_error_message().c_str(), pkt.pts * tb.num / tb.den, pkt.size, cbuf->get_total_packets(), cbuf->get_size());
			}
		}
		av_packet_unref(&pkt); // handle the release of the packet here
	}
}

int main(int argc, char** argv)
{
	// The IP camera
	string CameraPath = "rtsp://10.0.9.113:8554/0";
	CameraPath = "rtsp://10.0.9.111:554/user=admin_password=tlJwpbo6_channel=1_stream=0";
	string CameraName = "FrontCam";

	if (argc > 1)
	{
		CameraName.assign(argv[1]);
	}

	if (argc > 2)
	{
		CameraPath.assign(argv[2]);
	}

	fprintf(stderr, "Now starting the test %s on %s.\n", CameraName.c_str(), CameraPath.c_str());
	prefix_videofile.append(CameraName + "-"); // add the camera name to video file prefix

	ipCam = new Demuxer();

	// ip camera options
	ipCam->set_options("buffer_size", "200000");
	ipCam->set_options("rtsp_transport", "tcp");
	ipCam->set_options("stimeout", "100000");

	// USB camera options
	//ipCam->set_options("video_size", "1280x720");
	//ipCam->set_options("framerate", "30");
	//ipCam->set_options("vcodec", "h264");

	int ret = ipCam->open(CameraPath);
	if (ret < 0)
	{
		fprintf(stderr, "Could not open IP camera at %s with error %s.\n", CameraPath.c_str(), ipCam->get_error_message().c_str());
		exit(1);
	}
	AVStream* input_stream = ipCam->get_stream(ipCam->get_video_index());

	// Debug only, output the camera information
	if (Debug > 0)
	{
		av_dump_format(ipCam->get_input_format_context(), 0, CameraPath.c_str(), 0);
	}

	// Open a circular buffer
	cbuf = new CircularBuffer();
	cbuf->open(30, 100 * 1000 * 1000); // set the circular buffer to be hold packets for 30s and maximum size 100M
	cbuf->add_stream(input_stream);

	Muxer* bg_recorder = new Muxer();
	int bg_video_muxer_index = bg_recorder->add_stream(input_stream);

	Muxer* mn_recorder = new Muxer();
	int mn_video_muxer_index = mn_recorder->add_stream(input_stream);

	// Start a seperate thread to capture video stream from the IP camera
	//pthread_t thread;
	//ret = pthread_create(&thread, NULL, videoCapture, NULL);
	DWORD myThreadID;
	HANDLE myHandle = CreateThread(0, 0, videoCapture, 0, 0, &myThreadID);
	if (myThreadID == 0)
	{
		fprintf(stderr, "Cannot create the timer thread.");
		exit(1);
	}
	av_usleep(10 * 1000 * 1000); // sleep for a while to have the circular buffer accumulated

	AVPacket pkt;
	AVRational timebase = cbuf->get_time_base();
	int64_t pts0 = 0;
	int64_t last_pts = 0;
	bool no_data = true;
	bool main_recorder_recording = false;

	bg_recorder->set_options("movflags", "frag_keyframe");
	bg_recorder->set_options("format", "mp4"); // self defined option

	mn_recorder->set_options("movflags", "frag_keyframe");

	// Open a chunked recording for background recording, where chunk time is 60s
	ret = bg_recorder->open(prefix_videofile + "background-", 60);
	//av_dump_format(bg_recorder->get_output_format_context(), 0, bg_recorder->get_url().c_str(), 1);

	int64_t MainStartTime = av_gettime() / 1000 + 15000;
	int64_t ChunkTime_bg = 0;  // Chunk time for background recording
	int64_t ChunkTime_mn = 0;  // Chunk time for main recording
	int64_t CurrentTime = MainStartTime - 100;
	while (true)
	{
		CurrentTime = av_gettime() / 1000;  // read current time in miliseconds
		no_data = true;

		// read a background packet from the queue
		ret = cbuf->peek_packet(&pkt);
		if (ret > 0)
		{
			if (pts0 == 0)
			{
				pts0 = pkt.pts;
				last_pts = pkt.pts;
				if (Debug > 1)
				{
					fprintf(stderr, "The first packet: pts=%lld, pts_time=%lld \n",
						pts0, pts0 * timebase.num / timebase.den);
				}
			}

			if (Debug > 2)
			{
				fprintf(stderr, "Read a background packet pts time: %lldms, dt: %lldms, packet size %d, total packets: %d.\n",
					1000 * pkt.pts * timebase.num / timebase.den,
					1000 * (pkt.pts - pts0) * timebase.num / timebase.den, pkt.size, cbuf->get_total_packets());
			}

			if (pkt.pts == AV_NOPTS_VALUE || pkt.size == 0 || pkt.pts < last_pts)
			{
				fprintf(stderr, "Read a wrong background packet pts time: %lldms, dt: %lldms, packet size %d, total size: %d.\n",
					1000 * pkt.pts * timebase.num / timebase.den,
					1000 * (pkt.pts - pts0) * timebase.num / timebase.den, pkt.size, cbuf->get_total_packets());
			}

			last_pts = pkt.pts;
			ret = bg_recorder->record(&pkt);
			
			// check for error
			if (ret < 0)
			{
				fprintf(stderr, "%s muxing packet (%lldms, %d) in %s.\n",
					bg_recorder->get_error_message().c_str(),
					1000 * (pkt.pts - pts0) * timebase.num / timebase.den, pkt.size,
					bg_recorder->get_url().c_str());
				break;
			}

			// check for chunk
			if (ret > 0)
			{
				av_dump_format(bg_recorder->get_output_format_context(), 0,
					bg_recorder->get_url().c_str(), 1);
			}
			no_data = false;
		}

		// arbitrary set main recording starts 15s later
		if (CurrentTime <= MainStartTime)
		{
			if (no_data)
			{
				av_usleep(1000 * 20); //sleep for 20ms
			}
			continue;
		}

		if (!main_recorder_recording)
		{
			ret = mn_recorder->open(prefix_videofile + "main-", 3600);
			av_dump_format(mn_recorder->get_output_format_context(), 0, mn_recorder->get_url().c_str(), 1);
			main_recorder_recording = true;
			ChunkTime_mn = CurrentTime + 60000;
		}

		// simulate the external chunk signal
		if (ChunkTime_mn && CurrentTime > ChunkTime_mn)
		{
			ChunkTime_mn += 120000;
			mn_recorder->chunk();
			av_dump_format(mn_recorder->get_output_format_context(), 0, mn_recorder->get_url().c_str(), 1);
			fprintf(stderr, "Main recording get chunked.\n");
		}

		// handle the main stream reading
		ret = cbuf->peek_packet(&pkt, false);
		if (ret > 0)
		{
			if (Debug > 2)
			{
				fprintf(stderr, "Read a main packet pts time: %lld, dt: %lldms, packet size %d, total size: %d.\n",
					pkt.pts * timebase.num / timebase.den,
					1000 * (pkt.pts - pts0) * timebase.num / timebase.den, pkt.size, ret);
			}

			if (mn_recorder->record(&pkt) < 0)
			{
				fprintf(stderr, "%s muxing packet in %s.\n",
					mn_recorder->get_error_message().c_str(),
					mn_recorder->get_url().c_str());
				break;
			}
			//av_packet_unref(&pkt);

			no_data = false;
		}

		// sleep to reduce the cpu usage
		if (no_data)
		{
			av_usleep(1000 * 20); // sleep for extra 20ms when there is no more background reading
		}
		else
		{
			av_usleep(1000 * 5); // sleep for 5ms}
		}
	}

	if (ret < 0)
		fprintf(stderr, " with error %s.\n", av_err(ret));

}

