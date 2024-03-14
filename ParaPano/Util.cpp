//
// Created by zhuoqunc on 12/02/17.
//

#include <iostream>
#include <fstream>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits>
#include <opencv2/cudawarping.hpp>
#include <omp.h>

#include "Util.h"
#include "filter.h"
#include "keyPointDetector.h"
#include "stitcher.h"
#include "cudaFilter.h"

using namespace cv;

extern int num_images;

Util::Util() {
    gaussian_pyramid_elapsed = 0.0;
    dog_pyramid_elapsed = 0.0;
    keypoint_detection_elapsed = 0.0;
    compute_brief_elapsed = 0.0;
    find_match_elapsed = 0.0;
    compute_homography_elapsed = 0.0;
    stitching_elapsed = 0.0;
}

Util::~Util() {}

void Util::printTiming() const {
    printf("Compute Gaussian Pyramid: %.2f\n", gaussian_pyramid_elapsed);
    printf("Compute DoG Pyramid: %.2f\n", dog_pyramid_elapsed);
    printf("Detect Keypoints: %.2f\n", keypoint_detection_elapsed);
    printf("Compute BRIEF Descriptor: %.2f\n", compute_brief_elapsed);
    printf("Match keypoint descriptors: %.12f\n", find_match_elapsed);
    //printf("Compute Homography: %.2f\n", compute_homography_elapsed);
    printf("Stitch Images: %.2f\n", stitching_elapsed);
}

inline double Util::get_time_elapsed(clock_t& start)
{
    return (clock() - start) / (double)CLOCKS_PER_SEC;
}

bool Util::readImage(std::string im_name, Mat& im) {
    im = imread(im_name, IMREAD_COLOR);
    if(!im.data ) {
        std::cout <<  "Could not open or find the image " +
                        im_name << std::endl ;
        return false;
    }
    return true;
}

void Util::cleanPointerArray(float** arr, int num_levels) {
    for (int i = 0; i < num_levels; i++) {
        delete[] arr[i];
    }
    delete[] arr;
}

Mat Util::computeHomography(std::string im1_name, std::string im2_name,
                      BriefResult brief_result1, BriefResult brief_result2, int index) {

    std::cout << "Matching key points: " + im1_name + ", " + im2_name << std::endl;
    find_match_start = clock();

    MatchResult match = cudaBriefMatch(brief_result1.descriptors, brief_result2.descriptors);

    plotMatches(im1_name, im2_name, brief_result1.keypoints, 
                 brief_result2.keypoints, match, index);

    std::vector<Point> pts1;
    pts1.reserve(match.indices1.size());
    std::vector<Point> pts2;
    pts2.reserve(match.indices2.size());
    for (int i = 0; i < match.indices1.size(); i++) {
        int idx1 = match.indices1[i];
        int idx2 = match.indices2[i];
        pts1.push_back(brief_result1.keypoints[idx1]);
        pts2.push_back(brief_result2.keypoints[idx2]);
    }
    find_match_elapsed += get_time_elapsed(find_match_start);

    std::cout << "Computing Homography..." << std::endl;
    compute_homography_start = clock();
    Mat H = findHomography(pts2, pts1, RANSAC, 4.0);
    compute_homography_elapsed += get_time_elapsed(compute_homography_start);

    H.convertTo(H,CV_32F);
    return H;
}


BriefResult Util::BriefLite(std::string im_name, Point* compareA,
                            Point* compareB) {

    std::cout << "Computing BRIEF for image " + im_name << std::endl;

    Mat im_color = imread(im_name, IMREAD_COLOR);

    // Load as grayscale and convert to float
    Mat im_gray = imread(im_name, IMREAD_GRAYSCALE);
    Mat im;
    im_gray.convertTo(im, CV_32F);
    int h = im.rows;
    int w = im.cols;
    float *im1_ptr = (float*) im.ptr<float>();
    normalize_img(im1_ptr, h, w);

    // parameters for generating Gaussian Pyramid
    float sigma0 = 1.0;
    float k = sqrt(2);
    int num_levels = 7;
    int levels[7] = {-1, 0, 1, 2, 3, 4, 5};

    CudaFilterer cudaFilterer;
    cudaFilterer.setup(im1_ptr, h, w);

    // float** gaussian_pyramid_cuda = 
    //     cudaFilterer.createGaussianPyramid(sigma0, k, levels, num_levels);

    gaussian_pyramid_start = clock();
    float** gaussian_pyramid = 
        cudaFilterer.createGaussianPyramid(sigma0, k, levels, num_levels);
    
    //float** gaussian_pyramid = createGaussianPyramid(im1_ptr, h, w, sigma0, k,
                                                     // levels, num_levels);                    
    gaussian_pyramid_elapsed += get_time_elapsed(gaussian_pyramid_start);


    dog_pyramid_start = clock();
    float** dog_pyramid = createDoGPyramid(gaussian_pyramid, h, w, num_levels);
    // outputGaussianImages(gaussian_pyramid, h, w, num_levels);
    //outputDoGImages(dog_pyramid, h, w, num_levels);

    dog_pyramid_elapsed += get_time_elapsed(dog_pyramid_start);

    keypoint_detection_start = clock();
    // Detect key points
    float th_contrast = 0.03;
    float th_r = 12;
    std::vector<Point> keypoints = getLocalExtrema(dog_pyramid, num_levels - 1,
                                                   h, w, th_contrast, th_r);
    printf("Detected %lu key points\n", keypoints.size());
    keypoint_detection_elapsed += get_time_elapsed(keypoint_detection_start);


    //outputImageWithKeypoints(im_name, im_color, keypoints);

    compute_brief_start = clock();
    BriefResult brief_result = computeBrief(gaussian_pyramid[0], h, w, 
                                            keypoints, compareA, compareB);
    compute_brief_elapsed += get_time_elapsed(compute_brief_start);


    // clean up
    cleanPointerArray(gaussian_pyramid, num_levels);
    cleanPointerArray(dog_pyramid, num_levels - 1);

    return brief_result;
}

void Util::plotMatches(std::string im1_name, std::string im2_name,
                       std::vector<Point>& pts1, std::vector<Point>& pts2,
                       MatchResult& match, int index) {

    Mat im1 = imread(im1_name, IMREAD_COLOR);
    Mat im2 = imread(im2_name, IMREAD_COLOR);
    int h1 = im1.rows;
    int w1 = im1.cols;
    int h2 = im2.rows;
    int w2 = im2.cols;
    int width = w1 + w2;
    int height = max(h1, h2);
    Mat grid(height, width, CV_8UC3, Scalar(0,0,0));
    im1.copyTo(grid(Rect(0,0,w1,h1)));
    im2.copyTo(grid(Rect(w1,0,w2,h2)));

    for (int i = 0; i < match.indices1.size(); i++) {
        Point p1 = pts1[match.indices1[i]];
        Point p2 = pts2[match.indices2[i]];
        line(grid, p1, Point(p2.x+w1, p2.y), Scalar(0, 0, 255));
    }

    std::cout << "Output Match Image" << std::endl;
    // imwrite("../result_imgs/"+std::__cxx11::to_string(index)+"_match.jpg" , grid);
}

void Util::stitch(std::vector<Mat> images, std::vector<Mat> homographies, int width, int height) {
    stitching_start = clock();

    Mat panorama = Mat::zeros(height, width, images[0].type());
        // std::cout << "image type " << images[0].type();  

    panorama = panorama / 255;
    // imwrite("../result_imgs/panorama_intro.jpg", panorama * 255);

    Mat pano_mask = Mat::zeros(height, width, images[0].type());
    // Mat result = Mat::zeros(height, width, images[0].type());

    for (int i = 0; i < num_images; i++) {
    //   std::cout << i << std::endl;
        // create mask for image
        Mat img_mask = createMask(images[i]);
        std::string output_name = "../result_imgs/in_img_mask_" + std::to_string(i) + ".jpg";
        // imwrite(output_name, img_mask);
    //   std::cout << "----------------------------" << std::endl;

        cuda::GpuMat src_gpu, dst_gpu;
    //   std::cout << "----------------------------" << std::endl;

        src_gpu.upload(images[i]);
        // dst_gpu.upload(img_mask);
        //   std::cout << "----------------------------" << std::endl;

        // cuda::warpPerspective(src_gpu, dst_gpu, homographies[i], Size(width, height));
        cuda::warpPerspective(src_gpu, dst_gpu, homographies[i], Size(width, height));
    //   std::cout << "----------------------------" << std::endl;

        dst_gpu.download(img_mask);
       
    //   std::cout << "----------------------------" << std::endl;

        // cv::imwrite( std::__cxx11::to_string(i)+"_half.jpg", my_result*255);
        // if ( i == 0) {
        //     pano_mask = pano_mask + img_mask;
        // }
        // else {
        // cv::Mat half(img_mask,cv::Rect(0,0,pano_mask.cols,pano_mask.rows));
        // cv::imwrite( std::__cxx11::to_string(i)+"check.jpg", half*255);

        // pano_mask.copyTo(half);    
        // pano_mask = img_mask.clone();
        // cv::imwrite( "../result_imgs/out_img_mask_" + std::to_string(i) + ".jpg", img_mask*255);

        // }
        
        // output_name = "../result_imgs/out_img_mask_" + std::to_string(i) + ".jpg";
 
        
        // output_name = "../result_imgs/test_stitch_" + std::to_string(i) + ".jpg";
        // // imwrite(output_name, temp*255 );
    
    // #pragma omp parallel for
    for (int i = 1; i < pano_mask.rows-1; ++i) {
        
        // #pragma omp parallel for

        for (int j = 1; j < pano_mask.cols*3-1; ++j) {
            if (pano_mask.at<float>(i,j) ==0 && img_mask.at<float>(i,j) != 0.0 ||  pano_mask.at<float>(i,j) < 0.2 && img_mask.at<float>(i,j)/  pano_mask.at<float>(i,j) > 1.5) {
                    pano_mask.at<float>(i,j) = img_mask.at<float>(i,j);
            }
            
            
            //  else {
            //         pano_mask.at<float>(i,j) = ((float)pano_mask.at<float>(i,j) + (float)img_mask.at<float>(i,j))
            //  }
        
    }

    }
        // pano_mask = pano_mask + img_mask   ;
        // result = result + bim5;
        // imwrite("/home/ntduc/Downloads/test_mask_"+std::to_string(i)+".jpg", pano_mask * 255);
        // imwrite("/home/ntduc/Downloads/result_temp_"+std::to_string(i)+".jpg", result * 255);

        // cv::imwrite( std::__cxx11::to_string(i)+"_half.jpg", my_result*255);
        // output_name = "../result_imgs/stitch_" + std::to_string(i) + ".jpg";
        // imwrite(output_name, pano_mask * 255);

    }
    stitching_elapsed = get_time_elapsed(stitching_start);
    std::string user_name = "ntduc";
    //displayImg(panorama);
    imwrite("/home/"+user_name+"/Downloads/result.jpg", pano_mask * 255);
}

int Util::readTestPattern(Point*& compareA, Point*& compareB,
                          std::string test_pattern_filename) {

    std::ifstream infile(test_pattern_filename);
    int num_test_pairs;
    infile >> num_test_pairs;
    // std::cout << "so cap tesst day ne: " << num_test_pairs;

    compareA = new Point[num_test_pairs];
    compareB = new Point[num_test_pairs];
    int x1, y1, x2, y2;
    for (int i = 0; i < num_test_pairs; i++) {
        infile >> x1 >> y1 >> x2 >> y2;
        compareA[i] = Point(x1, y1);
        compareB[i] = Point(x2, y2);
    }
    return num_test_pairs;
}

void Util::printImage(float* img, int h, int w) const {
    for (int i = 0; i < h; i++) {
        
        for (int j = 0; j < w; j++) {
            std::cout << img[i * w + j] << " ";
        }
        std::cout << std::endl;
    }
}

inline void Util::displayImg(Mat& im) const {
    // Create a window for display.
    namedWindow( "Display window", 0);
    resizeWindow("Display window", im.cols, im.rows);
    imshow( "Display window", im);           // Show our image inside it.
    waitKey(0);
}
