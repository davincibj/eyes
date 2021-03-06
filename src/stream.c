#include "network.h"
#include "detection_layer.h"
#include "region_layer.h"
#include "cost_layer.h"
#include "utils.h"
#include "parser.h"
#include "box.h"
#include "image.h"
#include "stream.h"
#include <sys/time.h>
#include "zmq.h"

#ifdef OPENCV
#include "opencv2/highgui/highgui_c.h"
#include "opencv2/imgproc/imgproc_c.h"
image get_image_from_raw_data(IplImage* src);

static char **demo_names;
static image **demo_alphabet;
static int demo_classes;

static float **probs;
static box *boxes;
static network net;
static image in   ;
static image in_s ;

static float demo_thresh = .24;
static float demo_hier_thresh = .5;
static unsigned char rcv_buf[1024*768*3*3];
static int zmq_addr_len;

void get_Iplimage_from_raw_data(IplImage* imgShow, unsigned char* raw_data, int data_len)
{
	CvMat tmpMat;
	memcpy(imgShow->imageData, raw_data, data_len);
	imgShow->imageSize= data_len;
	CvMat *tmpMatptr = cvGetMat( imgShow, &tmpMat, NULL, 0 );
	imgShow = cvDecodeImage(tmpMatptr, CV_LOAD_IMAGE_COLOR);
	in = get_image_from_raw_data(imgShow);
	in_s = resize_image(in, net.w, net.h);
	cvReleaseMat(&tmpMatptr);
	cvReleaseImage(&imgShow);
	return;
}

void fetch_data_zmq(void *dealer, IplImage* imgShow)
{
	int more = 0;

	int len = 0;
	size_t opt_size = sizeof (len);
	unsigned char* raw_data_pos = NULL;
	zmq_addr_len = 0;
	do{
		int rc = zmq_recv (dealer, rcv_buf+len, sizeof(rcv_buf) - len,0);//, ZMQ_NOBLOCK);
		assert (rc != -1);
		if (more == 0){ //first come, it is identity
			raw_data_pos = rcv_buf+len+rc;
			zmq_addr_len = rc;
		}
		len += rc;
		rc = zmq_getsockopt (dealer, ZMQ_RCVMORE, &more, &opt_size);
		assert (rc == 0);

	}while(more);
	get_Iplimage_from_raw_data(imgShow, raw_data_pos, len - (raw_data_pos - rcv_buf));
	return;
}

void detect_data_zmq(image im)
{
    float nms=.4;
    layer l = net.layers[net.n-1];
	float *X = im.data;
	network_predict(net, X);
	int show_flag = 0;

	if(l.type == DETECTION){
		get_detection_boxes(l, 1, 1, demo_thresh, probs, boxes, 0);
	} else if (l.type == REGION){
		get_region_boxes(l, 1, 1, demo_thresh, probs, boxes, 0, 0, demo_hier_thresh);
	} else {
		error("Last layer must produce detections\n");
	}
	if (l.softmax_tree && nms)
		do_nms_obj(boxes, probs, l.w*l.h*l.n, l.classes, nms);
	else if(nms) do_nms_sort(boxes, probs, l.w*l.h*l.n, l.classes, nms);

	draw_detections(im, l.w*l.h*l.n, 0.24, boxes, probs, demo_names, demo_alphabet, l.classes, show_flag);
	return ;
}

void push_data_zmq(void *dealer, image im)
{
	char zmq_addr [100];
	/*'Stream-in-%d'*/
	char zmq_prefix[] = "Stream-in-";
	memcpy(zmq_addr, rcv_buf+sizeof(zmq_prefix)-1, zmq_addr_len - (sizeof(zmq_prefix)-1));
	zmq_addr[zmq_addr_len - (sizeof(zmq_prefix)-1)] = '\0';
	int client_id = atoi(zmq_addr);
	int encode_param[3];
	encode_param[0] = CV_IMWRITE_JPEG_QUALITY;
	encode_param[1] = 85;
	encode_param[2] = 0;
	IplImage* iplImage = get_image_cv(im);
	CvMat* im_s = cvEncodeImage(".jpg", iplImage, encode_param);

	snprintf (zmq_addr, sizeof(zmq_addr), "Stream-out-%d", client_id);
	int rc = zmq_send(dealer, zmq_addr, strlen(zmq_addr), ZMQ_SNDMORE);
	assert (rc > 0);

	rc = zmq_send(dealer, im_s->data.ptr, im_s->step, ZMQ_NOBLOCK);
	assert (rc > 0);
	cvReleaseImage(&iplImage);
	cvReleaseMat(&im_s);
}
void stream(int gpu_id, char *cfgfile, char *weightfile, const char *ip_addr, const int port, char **names, int classes, float hier_thresh, float thresh)
{
    //skip = frame_skip;
    image **alphabet = load_alphabet();
    demo_names = names;
    demo_alphabet = alphabet;
    demo_classes = classes;
    demo_thresh = thresh;
    demo_hier_thresh = hier_thresh;
    printf("Stream\n");
    net = parse_network_cfg(cfgfile);
    if(weightfile){
        load_weights(&net, weightfile);
    }
    set_batch_network(&net, 1);
    srand(2222222);

    void *ctx   = zmq_ctx_new();
	void *dealer = zmq_socket(ctx,ZMQ_DEALER);
	char identity [20];
	snprintf (identity, sizeof(identity), "GPU-BROKER-%02d", gpu_id);

	char address [100];
	snprintf (address, sizeof(address), "tcp://%s:%d", ip_addr, port);
	zmq_setsockopt (dealer, ZMQ_IDENTITY, &identity, sizeof(identity));
	int rc = zmq_connect(dealer,address);
	assert(rc == 0);

    layer l = net.layers[net.n-1];
    int j;

    boxes = (box *)calloc(l.w*l.h*l.n, sizeof(box));
    probs = (float **)calloc(l.w*l.h*l.n, sizeof(float *));
    for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = (float *)calloc(l.classes, sizeof(float));

    int count = 0;
	int picH=480;
	int picW=640;
	IplImage* imgShow = cvCreateImageHeader(cvSize(picW, picH), IPL_DEPTH_8U, 3);
	cvCreateData(imgShow);
	cvZero(imgShow);

    while(1){
        ++count;
        if(1){
        	fetch_data_zmq(dealer,imgShow);
        	//det = in_s;
        	detect_data_zmq(in_s);
        	push_data_zmq(dealer, in_s);
            /*show_image(in_s, "Stream");
            int c = cvWaitKey(1);
            if (c == 'q')
            {
            	free_image(in);
            	break;
            }*/
            free_image(in);
            free_image(in_s);
            //free_image(disp);
        }
    }
    free(boxes);
	free_ptrs((void **)probs, l.w*l.h*l.n);
	cvReleaseImage(&imgShow);
}
#else
void stream(int gpu_id, char *cfgfile, char *weightfile, const char *ip_addr, const int port, char **names, int classes, float hier_thresh, float thresh)
{
    fprintf(stderr, "Demo needs OpenCV for webcam images.\n");
}
#endif

