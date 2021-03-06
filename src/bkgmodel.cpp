#include "bkgmodel.h"

BkgModel::BkgModel()
{
}


/*
This file is part of the OpenKinect Project. http://www.openkinect.org
Copyright (c) 2010 individual OpenKinect contributors. See the CONTRIB file
for details.
This code is licensed to you under the terms of the Apache License, version
2.0, or, at your option, the terms of the GNU General Public License,
version 2.0. See the APACHE20 and GPL2 files for the text of the licenses,
or the following URLs:
http://www.apache.org/licenses/LICENSE-2.0
http://www.gnu.org/licenses/gpl-2.0.txt
108
www.it-ebooks.info
CHAPTER 6  VOXELIZATION
* If you redistribute this file in source form, modified or unmodified, you
* may:
*
1) Leave this header intact and distribute it under the same terms,
*
accompanying it with the APACHE20 and GPL20 files, or
*
2) Delete the Apache 2.0 clause and accompany it with the GPL2 file, or
*
3) Delete the GPL v2 clause and accompany it with the APACHE20 file
* In all cases you must keep the copyright notice intact and include a copy
* of the CONTRIB file.
*
* Binary distributions must follow the binary distribution requirements of
* either License.
*/


#include <iostream>
#include <libfreenect.hpp>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <vector>
#include <ctime>
#include <boost/thread/thread.hpp>
#include "pcl/common/common_headers.h"
#include "pcl/features/normal_3d.h"
#include "pcl/io/pcd_io.h"
#include "pcl/visualization/pcl_visualizer.h"
#include "pcl/console/parse.h"
#include "pcl/point_types.h"
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/surface/mls.h>
#include "boost/lexical_cast.hpp"
#include "pcl/filters/voxel_grid.h"
#include "pcl/octree/octree.h"
#include <pcl/filters/radius_outlier_removal.h>
#include <libfreenect_registration.h>


///Mutex Class
class Mutex {
public:
    Mutex() {
        pthread_mutex_init( &m_mutex, NULL );
    }
    void lock() {
        pthread_mutex_lock( &m_mutex );
    }
    void unlock() {
        pthread_mutex_unlock( &m_mutex );
    }
    class ScopedLock
    {
        Mutex & _mutex;
    public:
        ScopedLock(Mutex & mutex)
            : _mutex(mutex)
        {
            _mutex.lock();
        }
        ~ScopedLock()
        {
            _mutex.unlock();
        }
    };
private:
    pthread_mutex_t m_mutex;
};


///Kinect Hardware Connection Class
/* thanks to Yoda---- from IRC */
class MyFreenectDevice : public Freenect::FreenectDevice {
public:
    MyFreenectDevice(freenect_context *_ctx, int _index)
        : Freenect::FreenectDevice(_ctx, _index),
          depth(freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_DEPTH_REGISTERED).bytes),
          m_buffer_video(freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM,
                                                  FREENECT_VIDEO_RGB).bytes), m_new_rgb_frame(false), m_new_depth_frame(false)
    {
    }
    //~MyFreenectDevice(){}
    // Do not call directly even in child
    void VideoCallback(void* _rgb, uint32_t timestamp) {
        Mutex::ScopedLock lock(m_rgb_mutex);
        uint8_t* rgb = static_cast<uint8_t*>(_rgb);
        std::copy(rgb, rgb+getVideoBufferSize(), m_buffer_video.begin());
        m_new_rgb_frame = true;
    };
    // Do not call directly even in child
    void DepthCallback(void* _depth, uint32_t timestamp) {
        Mutex::ScopedLock lock(m_depth_mutex);
        depth.clear();
        uint16_t* call_depth = static_cast<uint16_t*>(_depth);
        for (size_t i = 0; i < 640*480 ; i++) {
            depth.push_back(call_depth[i]);
        }
        m_new_depth_frame = true;
    }
    bool getRGB(std::vector<uint8_t> &buffer) {
        Mutex::ScopedLock lock(m_rgb_mutex);
        if (!m_new_rgb_frame)
            return false;
        buffer.swap(m_buffer_video);
        m_new_rgb_frame = false;
        return true;
    }

    bool getDepth(std::vector<uint16_t> &buffer) {
        Mutex::ScopedLock lock(m_depth_mutex);
        if (!m_new_depth_frame)
            return false;
        buffer.swap(depth);
        m_new_depth_frame = false;
        return true;
    }
private:
    std::vector<uint16_t> depth;
    std::vector<uint8_t> m_buffer_video;
    Mutex m_rgb_mutex;
    Mutex m_depth_mutex;
    bool m_new_rgb_frame;
    bool m_new_depth_frame;
};



///Start the PCL/OK Bridging
//OK
Freenect::Freenect freenect;
MyFreenectDevice* device;
freenect_video_format requested_format(FREENECT_VIDEO_RGB);
double freenect_angle(0);
int got_frames(0),window(0);
int g_argc;
char **g_argv;
int user_data = 0;
//PCL
pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZRGB>);
pcl::PointCloud<pcl::PointXYZRGB>::Ptr bgcloud (new pcl::PointCloud<pcl::PointXYZRGB>);
pcl::PointCloud<pcl::PointXYZRGB>::Ptr voxcloud (new pcl::PointCloud<pcl::PointXYZRGB>);
pcl::PointCloud<pcl::PointXYZRGB>::Ptr fgcloud (new pcl::PointCloud<pcl::PointXYZRGB>);
float resolution = 50.0; //5 CM voxels
// Instantiate octree-based point cloud change detection class
pcl::octree::OctreePointCloudChangeDetector<pcl::PointXYZRGB> octree (resolution);
//Instantiate octree point cloud
pcl::octree::OctreePointCloud<pcl::PointXYZRGB> octreepc(resolution);
bool BackgroundSub = false;
bool hasBackground = false;
bool grabBackground = false;
int bgFramesGrabbed = 0;
const int NUMBGFRAMES = 10;
bool Voxelize = false;
bool Oct = false;
bool Person = false;

unsigned int cloud_id = 0;
///Keyboard Event Tracking
void keyboardEventOccurred (const pcl::visualization::KeyboardEvent &event,
                            void* viewer_void)
{
    boost::shared_ptr<pcl::visualization::PCLVisualizer> viewer =
            *static_cast<boost::shared_ptr<pcl::visualization::PCLVisualizer> *> (viewer_void);
    if (event.getKeySym () == "c" && event.keyDown ())
    {
        std::cout << "c was pressed => capturing a pointcloud" << std::endl;
        std::string filename = "KinectCap";
        filename.append(boost::lexical_cast<std::string>(cloud_id));
        filename.append(".pcd");
        pcl::io::savePCDFileASCII (filename, *cloud);
        cloud_id++;
    }
    if (event.getKeySym () == "b" && event.keyDown ())
    {
        std::cout << "b was pressed" << std::endl;
        if (BackgroundSub == false)
        {
            //Start background subtraction
            if (hasBackground == false)
            {
                //Grabbing Background
                std::cout << "Starting to grab backgrounds!" << std::endl;
                grabBackground = true;
            }
            else
                BackgroundSub = true;
        }
        else
        {
            //Stop Background Subtraction
            BackgroundSub = false;
        }
    }
    if (event.getKeySym () == "v" && event.keyDown ())
    {
        std::cout << "v was pressed" << std::endl;
        Voxelize = !Voxelize;
    }
    if (event.getKeySym () == "o" && event.keyDown ())
    {
        std::cout << "o was pressed" << std::endl;
        Oct = !Oct;
        viewer->removeAllShapes();
    }

    if (event.getKeySym () == "p" && event.keyDown ())
    {
        std::cout << "p was pressed" << std::endl;
        Person = !Person;
        viewer->removeAllShapes();
    }
}


// --------------
// -----Main-----
// --------------
//int main (int argc, char** argv)
//{
//    //More Kinect Setup
//    static std::vector<uint16_t> mdepth(640*480);
//    static std::vector<uint8_t> mrgb(640*480*4);
//    // Fill in the cloud data
//    cloud->width
//            = 640;
//    cloud->height
//            = 480;
//    cloud->is_dense = false;
//    cloud->points.resize (cloud->width * cloud->height);
//    // Create and setup the viewer
//    boost::shared_ptr<pcl::visualization::PCLVisualizer> viewer (new
//                                                                 pcl::visualization::PCLVisualizer ("3D Viewer"));
//    viewer->registerKeyboardCallback (keyboardEventOccurred, (void*)&viewer);
//    viewer->setBackgroundColor (0, 0, 0);
//    viewer->addPointCloud<pcl::PointXYZRGB> (cloud, "Kinect Cloud");
//    viewer->setPointCloudRenderingProperties
//            (pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, "Kinect Cloud");
//    viewer->addCoordinateSystem (1.0);
//    viewer->initCameraParameters ();
//    //Voxelizer Setup
//    pcl::VoxelGrid<pcl::PointXYZRGB> vox;
//    vox.setLeafSize (resolution,resolution,resolution);
//    vox.setSaveLeafLayout(true);
//    vox.setDownsampleAllData(true);
//    pcl::VoxelGrid<pcl::PointXYZRGB> removeDup;
//    removeDup.setLeafSize (1.0f,1.0f,1.0f);
//    removeDup.setDownsampleAllData(true);
//    //Background Setup
//    pcl::RadiusOutlierRemoval<pcl::PointXYZRGB> ror;
//    ror.setRadiusSearch(resolution);
//    ror.setMinNeighborsInRadius(200);
//    device = &freenect.createDevice<MyFreenectDevice>(0);
//    device->startVideo();
//    device->startDepth();
//    boost::this_thread::sleep (boost::posix_time::seconds (1));

//    //Grab until clean returns
//    int DepthCount = 0;
//    while (DepthCount == 0) {
//        device->updateState();
//        device->getDepth(mdepth);
//        device->getRGB(mrgb);
//        for (size_t i = 0;i < 480*640;i++)
//            DepthCount+=mdepth[i];
//    }
//    //--------------------
//    // -----Main loop-----
//    //--------------------
//    double x = NULL;
//    double y = NULL;
//    int iRealDepth = 0;
//    while (!viewer->wasStopped ())
//    {
//        device->updateState();
//        device->getDepth(mdepth);
//        device->getRGB(mrgb);
//        size_t i = 0;
//        size_t cinput = 0;
//        for (size_t v=0 ; v<480 ; v++)
//        {
//            for ( size_t u=0 ; u<640 ; u++, i++)
//            {
//                iRealDepth = mdepth[i];
//                freenect_camera_to_world(device->getDevice(), u, v, iRealDepth, &x, &y);





//                freenect_camera_to_world(device->getDevice(),u,v,iRealDepth,&x,&y);
//                cloud->points[i].x = x;//1000.0;
//                cloud->points[i].y = y;//1000.0;
//                cloud->points[i].z = iRealDepth;//1000.0;
//                cloud->points[i].r = 255;//mrgb[i*3];
//                cloud->points[i].g = 255;//mrgb[(i*3)+1];
//                cloud->points[i].b = 255;//mrgb[(i*3)+2];
//            }
//        }
//        std::vector<int> indexOut;
//        pcl::removeNaNFromPointCloud (*cloud, *cloud, indexOut);
//        if (grabBackground) {
//            if (bgFramesGrabbed == 0)
//            {
//                std::cout << "First Grab!" << std::endl;
//                *bgcloud = *cloud;
//            }
//            else
//            {
//                //concat the clouds
//                *bgcloud+=*cloud;
//            }

//            bgFramesGrabbed++;
//            if (bgFramesGrabbed == NUMBGFRAMES) {
//                grabBackground = false;
//                hasBackground = true;
//                std::cout << "Done grabbing Backgrounds - hit b again to subtract BG." <<
//                             std::endl;
//                removeDup.setInputCloud (bgcloud);
//                removeDup.filter (*bgcloud);
//            }
//            else
//                std::cout << "Grabbed Background " << bgFramesGrabbed << std::endl;
//            viewer->updatePointCloud (bgcloud, "Kinect Cloud");
//        }
//        else if (BackgroundSub) {
//            octree.deleteCurrentBuffer();
//            fgcloud->clear();
//            // Add points from background to octree
//            octree.setInputCloud (bgcloud);
//            octree.addPointsFromInputCloud ();
//            // Switch octree buffers
//            octree.switchBuffers ();
//            // Add points from the mixed data to octree
//            octree.setInputCloud (cloud);
//            octree.addPointsFromInputCloud ();
//            std::vector<int> newPointIdxVector;
//            //Get vector of point indices from octree voxels
//            //which did not exist in previous buffer
//            octree.getPointIndicesFromNewVoxels (newPointIdxVector, 1);
//            for (size_t i = 0; i < newPointIdxVector.size(); ++i) {
//                fgcloud->push_back(cloud->points[newPointIdxVector[i]]);
//            }
//            //Filter the fgcloud down
//            ror.setInputCloud(fgcloud);
//            ror.filter(*fgcloud);
//            viewer->updatePointCloud (fgcloud, "Kinect Cloud");
//        }
//        else if (Voxelize) {
//            vox.setInputCloud (cloud);
//            vox.setLeafSize (5.0f, 5.0f, 5.0f);
//            vox.filter (*voxcloud);
//            viewer->updatePointCloud (voxcloud, "Kinect Cloud");
//        }
//        else
//            viewer->updatePointCloud (cloud, "Kinect Cloud");

//        viewer->spinOnce ();
//    }
//    device->stopVideo();
//    device->stopDepth();
//    return 0;
//}
