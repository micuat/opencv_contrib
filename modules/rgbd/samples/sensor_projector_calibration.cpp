/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000-2015, Intel Corporation, all rights reserved.
// Copyright (C) 2009-2011, Willow Garage Inc., all rights reserved.
// Copyright (C) 2015, OpenCV Foundation, all rights reserved.
// Copyright (C) 2015, Itseez Inc., all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include <opencv2/rgbd.hpp>

#include <opencv2/highgui.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
//#include <opencv2/videoio.hpp>
#include <opencv2/structured_light.hpp>
#include <opencv2/core/utility.hpp>

#include <iostream>
#include <fstream>
#include <cmath>

using namespace cv;
using namespace cv::rgbd;
using namespace cv::structured_light;
using namespace std;

int main(int argc, char** argv)
{
    Mat image, depth;
    float focalLength;

    VideoCapture capture(CAP_OPENNI2);

    if (!capture.isOpened())
    {
        cout << "OpenNI2 device unavailable" << endl;
        return -1;
    }

    // set registeration on
    capture.set(CAP_PROP_OPENNI_REGISTRATION, 0.0);

    // initialize gray coding
    GrayCodePattern::Params params;
    params.width = 1024;
    params.height = 768;
    Ptr<GrayCodePattern> pattern = GrayCodePattern::create(params);

    vector<Mat> patternImages;
    pattern->generate(patternImages, Scalar(0, 0, 0), Scalar(100, 100, 100));

    string window = "pattern";
    namedWindow(window, WINDOW_NORMAL);
    moveWindow(window, 0, 0);
    imshow(window, patternImages.at(0));

    // window placement; wait for user
    while (true)
    {
        int key = waitKey(30);
        if (key == 'f')
        {
            // TODO: 1px border when fullscreen on Windows (Surface?)
            setWindowProperty(window, WND_PROP_FULLSCREEN, WINDOW_FULLSCREEN);
        }
        else if (key == 'w')
        {
            setWindowProperty(window, WND_PROP_FULLSCREEN, WINDOW_NORMAL);
        }
        else if (key == ' ')
        {
            break;
        }
    }

    // capture
    vector<Mat> cameraImages;
    Mat depthAvg;

    for (size_t i = 0; i < patternImages.size(); i++) {
        imshow(window, patternImages.at(i));

        waitKey(500);

        capture.grab();

        capture.retrieve(image, CAP_OPENNI_BGR_IMAGE);
        capture.retrieve(depth, CAP_OPENNI_DEPTH_MAP);

        flip(image, image, 1);
        flip(depth, depth, 1);

        Mat gray;
        cvtColor(image, gray, COLOR_BGR2GRAY);
        cameraImages.push_back(gray);
        depth.convertTo(depth, CV_32F);
        depth = depth * 0.001f; // openni is in [mm]; there is a util function in rgbd though...
        if (depthAvg.empty())
        {
            depthAvg = depth;
        }
        else
        {
            depthAvg += depth;
        }
    }
    depthAvg *= 1.0f / static_cast<float>(patternImages.size());

    // prepare depth points
    focalLength = static_cast<float>(capture.get(CAP_PROP_OPENNI_FOCAL_LENGTH));

    // kinect parameter to focal length
    // https://github.com/OpenKinect/libfreenect/blob/master/src/registration.c#L317
    float fx = focalLength,
        fy = focalLength,
        cx = 319.5f,
        cy = 239.5f;

    Ptr<RgbdFrame> frame = makePtr<RgbdFrame>(image, depthAvg);

    frame->cameraMatrix = Mat::eye(3, 3, CV_32FC1);
    {
        frame->cameraMatrix.at<float>(0, 0) = fx;
        frame->cameraMatrix.at<float>(1, 1) = fy;
        frame->cameraMatrix.at<float>(0, 2) = cx;
        frame->cameraMatrix.at<float>(1, 2) = cy;
    }

    depthTo3d(*frame);

    // generate valid point mask for clusters
    compare(frame->depth, 0, frame->mask, CMP_GT);

    // decode
    vector<Point3d> objectPoints;
    vector<Point2d> imagePoints;
    Mat correspondenceMap = Mat::zeros(image.size(), CV_8UC3);
    for (int y = 0; y < image.rows; y++)
    {
        for (int x = 0; x < image.cols; x++)
        {
            if (frame->mask.at<uchar>(y, x) == 0)
            {
                continue;
            }

            Point point;
            if (pattern->getProjPixel(cameraImages, x, y, point) != true)
            {
                Point3f p3d = frame->points3d.at<Point3f>(y, x);
                objectPoints.push_back(Point3d(p3d.x, p3d.y, p3d.z));
                imagePoints.push_back(Point2d(point.x, point.y));
                Range xr(x, x + 1);
                Range yr(y, y + 1);
                correspondenceMap(yr, xr) = Scalar(point.x * 255 / params.width, point.y * 255 / params.height, 0);
            }
        }
    }
    imshow("correspondence", correspondenceMap);

    Mat projectorMatrix = Mat::eye(3, 3, CV_64FC1);
    {
        projectorMatrix.at<double>(0, 0) = 1500;
        projectorMatrix.at<double>(1, 1) = 1500;
        projectorMatrix.at<double>(0, 2) = params.width * 0.5f - 0.5f;
        projectorMatrix.at<double>(1, 2) = params.height * 1.2f - 0.5f; // vertical lens shift
    }
    // distCoeffs zero unless calibrated beforehand
    Mat distCoeffs = Mat::zeros(4, 1, CV_64FC1);
    Mat rvec, tvec;
    Rodrigues(Mat::eye(3, 3, CV_64FC1), rvec);
    tvec = Mat::zeros(3, 1, CV_64FC1);
    // TODO: replace with calibrateCamera
#if 1
    vector<int> inliers;
    solvePnPRansac(objectPoints, imagePoints, projectorMatrix, distCoeffs, rvec, tvec, true, 100000, 5, 0.999999999, inliers);
    vector<Point2d> repPoints;
    projectPoints(objectPoints, rvec, tvec, projectorMatrix, distCoeffs, repPoints);
    for(size_t i = 0; i < repPoints.size(); i++)
    {
        cout << norm(repPoints.at(i) - imagePoints.at(i)) << " ";
    }
    cout << endl;
    for(size_t i = 0; i < inliers.size(); i++)
    {
        cout << norm(repPoints.at(inliers.at(i)) - imagePoints.at(inliers.at(i))) << " ";
    }
#else
    vector<vector<Point3f> > objectPointsArray;
    vector<vector<Point2f> > imagePointsArray;
    objectPointsArray.push_back(objectPoints);
    imagePointsArray.push_back(imagePoints);
    vector<Mat> rvecs, tvecs;
    int flags = CALIB_USE_INTRINSIC_GUESS | CALIB_FIX_PRINCIPAL_POINT | CALIB_FIX_ASPECT_RATIO | CALIB_ZERO_TANGENT_DIST
        | CALIB_FIX_K1 | CALIB_FIX_K2 | CALIB_FIX_K3 | CALIB_FIX_K4;
    cout << calibrateCamera(objectPointsArray, imagePointsArray, Size(params.width, params.height * 2), projectorMatrix, distCoeffs, rvecs, tvecs, flags) << endl;
    rvec = rvecs.at(0);
    tvec = tvecs.at(0);
#endif

    cout << projectorMatrix << endl;
    cout << rvec << endl;
    cout << tvec << endl;
    cout << objectPoints.size() << endl;
    cout << imagePoints.size() << endl;
    // Rodrigues to form extrinsic matrix
    Mat rotation;
    Rodrigues(rvec, rotation);
    Mat extrinsics = Mat::eye(4, 4, CV_64FC1);
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            extrinsics.at<double>(i, j) = rotation.at<double>(i, j);
        }
        extrinsics.at<double>(i, 3) = tvec.at<double>(i);
    }

    // file output
    cout << extrinsics << endl;

    FileStorage fs("calibration.xml", FileStorage::WRITE);
    fs << "ProjectorCameraEnsemble";
    fs << "{";
    {
        fs << "name" << "OpenCV calibration";
        fs << "projectors";
        fs << "{";
        {
            fs << "Projector";
            fs << "{";
            {
                fs << "name" << 0;
                fs << "hostNameOrAddress" << "localhost";
                fs << "displayIndex" << 0;
                fs << "width" << params.width;
                fs << "height" << params.height;
                fs << "cameraMatrix";
                fs << "{";
                {
                    fs << "ValuesByColumn";
                    fs << "{";
                    for (int j = 0; j < 3; j++)
                    {
                        fs << "ArrayOfDouble";
                        fs << "{";
                        for (int i = 0; i < 3; i++)
                        {
                            fs << "double" << projectorMatrix.at<double>(i, j);
                        }
                        fs << "}";
                    }
                    fs << "}";
                }
                fs << "}";
                fs << "lensDistortion";
                fs << "{";
                {
                    fs << "ValuesByColumn";
                    fs << "{";
                    {
                        fs << "ArrayOfDouble";
                        fs << "{";
                        for (int i = 0; i < 2; i++)
                        {
                            fs << "double" << 0;
                        }
                        fs << "}";
                    }
                    fs << "}";
                }
                fs << "}";
                fs << "pose";
                fs << "{";
                {
                    fs << "ValuesByColumn";
                    fs << "{";
                    for (int j = 0; j < 4; j++)
                    {
                        fs << "ArrayOfDouble";
                        fs << "{";
                        for (int i = 0; i < 4; i++)
                        {
                            fs << "double" << extrinsics.at<double>(i, j);
                        }
                        fs << "}";
                    }
                    fs << "}";
                }
                fs << "}";
            }
            fs << "}";
        }
        fs << "}";
    }
    fs << "}";

    waitKey(0);
    return 0;
}
