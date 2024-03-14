#include <string>
#include <math.h>
#include <stdio.h>
#include <vector>
#include "device_launch_parameters.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <driver_functions.h>

#include "cudaFilter.h"

#define BLOCK_WIDTH 32
#define BLOCK_HEIGHT 16

using namespace std;

CudaFilterer::CudaFilterer() {
    gaussian_pyramid = NULL; // result on CPU
    cudaImageData = NULL;
    cudaGaussianPyramid = NULL;
    imageWidth = 0;
    imageHeight = 0;
    numLevels = 0;
}

CudaFilterer::~CudaFilterer() {
    if (cudaImageData) {
        // free image data on CUDA
        cudaFree(cudaImageData);
    }
}

void
printCudaInfo() {
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);

    printf("---------------------------------------------------------\n");
    printf("Found %d CUDA devices\n", deviceCount);

    for (int i=0; i<deviceCount; i++) {
        cudaDeviceProp deviceProps;
        cudaGetDeviceProperties(&deviceProps, i);
        printf("Device %d: %s\n", i, deviceProps.name);
        printf("   SMs:        %d\n", deviceProps.multiProcessorCount);
        printf("   Global mem: %.0f MB\n",
               static_cast<float>(deviceProps.totalGlobalMem) / (1024 * 1024));
        printf("   CUDA Cap:   %d.%d\n", deviceProps.major, deviceProps.minor);
    }
    printf("---------------------------------------------------------\n");
}

void
CudaFilterer::allocHostGaussianPyramid(int width, int height, int num_levels) {
    gaussian_pyramid = new float*[num_levels];
    for (int i = 0; i < num_levels; i++) {
        gaussian_pyramid[i] = new float[width * height];
    }
}

void
CudaFilterer::allocDeviceGaussianPyramid(int width, int height) {
    cudaMalloc(&cudaGaussianPyramid, sizeof(float) * width * height);
}

void
CudaFilterer::getGaussianPyramid(int i) {

    // need to copy contents of the rendered image from device memory
    // before we expose the Image object to the caller

    cudaMemcpy(gaussian_pyramid[i],
        cudaGaussianPyramid,
        sizeof(float) * imageWidth * imageHeight,
        cudaMemcpyDeviceToHost);
}

void
CudaFilterer::setup(float* img, int h, int w) {

    // printCudaInfo();

    // set parameters
    imageHeight = h;
    imageWidth = w;

    // copy image data from host to device
    cudaMalloc(&cudaImageData, sizeof(float) * w * h);
    cudaMemcpy(cudaImageData, img, sizeof(float) * w * h, cudaMemcpyHostToDevice);
}


// create a normalized gaussian filter of height h and width w
float*
createHostGaussianFilter(const int fh, const int fw, float sigma) {
    float* gaussianFilter = new float[fh * fw];
    float sum = 0.0;
    int centerX = fw/2;
    int centerY = fh/2;
    for (int i = 0; i < fh; i++) {
        for (int j = 0; j < fw; j++) {
            int x = j - centerX;
            int y = i - centerY;
            float e = -(x*x + y*y) / (2 * sigma * sigma);
            gaussianFilter[i * fw + j] = exp(e) / (2 * M_PI * sigma * sigma);
            sum += gaussianFilter[i * fw + j];
        }
    }

    // normalize
    for (int i = 0; i < fh; i++) {
        for (int j = 0; j < fw; j++) {
            gaussianFilter[i * fw + j] /= sum;
        }
    }
    return gaussianFilter;
}

__device__ __inline__ bool
inBound(int r, int c, int h, int w) {
    return r >= 0 && r < h && c >= 0 && c < w;
}

/*
 * kernel function
 */
__global__ void
applyGaussianFilter(const float* img_ptr, int h, int w, 
                    float* cudaFilter, int fsize, float* cudaGaussianPyramid) {

    int row = blockIdx.y * blockDim.y + threadIdx.y;
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    
    float weightedSum = 0.0;
    int fhHalf = fsize / 2;
    int fwHalf = fsize / 2;

    for (int ii = -fhHalf; ii < fsize - fhHalf; ii++) {
        for (int jj = -fwHalf; jj < fsize - fwHalf; jj++) {
            int r = row + ii;
            int c = col + jj;
            float imVal = inBound(r, c, h, w) ? img_ptr[r * w + c] : 0;
            weightedSum += imVal * cudaFilter[(ii+fhHalf)*fsize + (jj+fwHalf)];
        }
    }
    cudaGaussianPyramid[row * w + col] = weightedSum;
}
 
float**
CudaFilterer::createGaussianPyramid(float sigma0, float k, const int* levels,
                                    int num_levels) {

    numLevels = num_levels;

    // allocate host memory
    allocHostGaussianPyramid(imageWidth, imageHeight, num_levels);

    allocDeviceGaussianPyramid(imageWidth, imageHeight);

    for (int i = 0; i < num_levels; i++) {
        float sigma = sigma0 * pow(k, levels[i]);
        int fsize = floor(3 * sigma * 2) + 1;
        float* filter = createHostGaussianFilter(fsize, fsize, sigma);

        // copy filter to CUDA memory
        float* cudaFilter;
        cudaMalloc(&cudaFilter, sizeof(float) * fsize * fsize);
        cudaMemcpy(cudaFilter, filter, sizeof(float) * fsize * fsize,
                   cudaMemcpyHostToDevice);

        // Spawn CUDA threads
        dim3 gridDim(imageWidth / BLOCK_WIDTH, imageHeight / BLOCK_HEIGHT);
        dim3 blockDim(BLOCK_WIDTH, BLOCK_HEIGHT);

        applyGaussianFilter<<< gridDim, blockDim>>>(cudaImageData, imageHeight,
                        imageWidth, cudaFilter, fsize, cudaGaussianPyramid);

        // Transfer the ith pyramid from device to host's gaussian_pyramid
        getGaussianPyramid(i);

        // clean up memory
        delete[] filter;
        cudaFree(cudaFilter);
    }
    cudaFree(cudaGaussianPyramid);

    return gaussian_pyramid;
}
