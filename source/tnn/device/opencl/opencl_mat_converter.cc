// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "tnn/device/opencl/opencl_mat_converter.h"
#include "tnn/core/macro.h"
#include "tnn/device/opencl/opencl_utils.h"
#include "tnn/memory_manager/blob_memory_size_info.h"
#include "tnn/utils/blob_memory_size_utils.h"
#include "tnn/utils/dims_vector_utils.h"
#include "tnn/utils/string_utils.h"

namespace TNN_NS {

Status OpenCLMatConverterAcc::Copy(Mat& src, Mat& dst, void* command_queue) {
    Status ret           = TNN_OK;
    //printf("\n log_copy \n ");
    //BlobMemorySizeInfo size_info = Calculate2DCLImageMemorySize(blob->GetBlobDesc());
    // force float to get the max memeory
    bool copy_flag = false;
    if (src.GetDeviceType() != DEVICE_OPENCL) {//CPU -> GPU
        copy_flag = false;
    } else if (dst.GetDeviceType() != DEVICE_OPENCL){//GPU->CPU
        copy_flag = true;
    }
    printf("copy_flag: %d", copy_flag);
    // buffer_reset
    BlobMemorySizeInfo info;
    info.data_type = DATA_TYPE_FLOAT;
    int batch, channel, height, width;
    batch            = src.GetBatch();
    channel          = src.GetChannel();
    height           = src.GetHeight();
    width            = src.GetWidth();
    //nchw->nhwc
    int image_width  = UP_DIV(channel, 4) * width;
    int image_height = batch * height;
    info.dims.push_back(image_width);
    info.dims.push_back(image_height);

    info.data_type   = DATA_TYPE_FLOAT;
    auto opencl_runtime   = OpenCLRuntime::GetInstance();
    buffer_size_          = GetBlobMemoryBytesSize(info);
    cl_int ret_cl            = CL_SUCCESS;
    cl::Buffer* cl_buffer = new cl::Buffer(*opencl_runtime->Context(), CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR,
                                            (cl::size_type)buffer_size_, nullptr, &ret_cl);
    if (ret_cl != CL_SUCCESS) {
        CHECK_CL_SUCCESS(ret_cl)
        if (nullptr != cl_buffer)
            delete cl_buffer;
    } else {
        buffer_.reset(cl_buffer);
    }

    auto cl_command_queue = static_cast<cl::CommandQueue *>(command_queue);
    MatType src_mat_type = src.GetMatType();
    MatType dst_mat_type = dst.GetMatType();
    //mat_ = 
    if (cl_command_queue == nullptr) {
        LOGE("Get OpenCL command queue failed!\n");
        return Status(TNNERR_NULL_PARAM, "Get OpenCL command queue failed!");
    }

    if (nullptr == buffer_) {
        LOGE("OpenCLBlobConverter buffer allocate failed\n");
        return Status(TNNERR_NULL_PARAM, "OpenCLBlobConverter buffer allocate failed!");
    }

    if(src_mat_type != dst_mat_type){
        return Status(TNNERR_PARAM_ERR, "convert type not support yet");
    }

    //create identifier
    std::string mat_key = to_string(src.GetDeviceType()) + "_" + to_string(dst.GetDeviceType());
    //create convert unit only once for every key
    OpenCLExecuteUnit unit;
    // ret = CreateConvertUnit(unit, src, copy_flag);
    // if (ret != TNN_OK) {
    //     return ret;
    // }
    if(copy_flag){
        if(execute_map_.count(mat_key) == 0) {
            std::string program_name = "convert_to_mat";
            std::string kernel_name = "";
            if(N8UC4 == dst.GetMatType()) {
                kernel_name = "CopyToN8UC4";
            } else if (N8UC3 == dst.GetMatType()) {
                kernel_name = "CopyToN8UC3";
            }
            ret = CreateExecuteUnit(unit, program_name, kernel_name);
            if(ret != TNN_OK) {
                return ret;
            }
            execute_map_[mat_key] = unit; 
        }
    } else {
        if(execute_map_.count(mat_key) == 0) {
            std::string program_name = "convert_from_mat";
            std::string kernel_name = "";
            if(N8UC4 == src.GetMatType()) {
                kernel_name = "CopyFromN8UC4";
            } else if (N8UC3 == src.GetMatType()) {
                kernel_name = "CopyFromN8UC3";
            }
            ret = CreateExecuteUnit(unit, program_name, kernel_name);
            if(ret != TNN_OK) {
                return ret;
            }
            execute_map_[mat_key] = unit; 
        }
    }

    // execute_map_[mat_key] = unit;
    // OpenCLExecuteUnit unit = execute_map_[mat_key];

    // set arguments
    ret                    = SetConvertArgs(unit, src, dst, false);
    if (ret != TNN_OK) {
        return ret;
    }

    //if src device is cpu, need copy src_mat data to buffer and bind buffer to dst_mat data
    if (src.GetDeviceType() != DEVICE_OPENCL) {
        ret = CopyMatToBufferData(src, cl_command_queue);
        if (ret != TNN_OK) {
            return ret;
        }
        ret = RunConvertUnit(unit, cl_command_queue, false);
        if (ret != TNN_OK) {
            return ret;
        }
    } else {
        ret = RunConvertUnit(unit, cl_command_queue, false);
        if (ret != TNN_OK) {
            return ret;
        }
        ret = CopyBufferDataToMat(dst, cl_command_queue);
        if (ret != TNN_OK) {
            return ret;
        }
    }
    return ret;

}

//enqueueMapBuffer get cpu buffer pointer, copy buffer pointer to mat, enqueueUnmapMemObject.
Status OpenCLMatConverterAcc::CopyBufferDataToMat(Mat &mat, cl::CommandQueue *command_queue) {
    MatType mat_type   = mat.GetMatType();
    DimsVector dims    = mat.GetDims();
    int data_type_size = 1;
    if (mat_type == NCHW_FLOAT) {
        data_type_size = sizeof(float);
    } else if (mat_type == N8UC4) {
        //special for 8UC4, blob channel <= 4.
        dims[1] = 4;
    }
    int size_in_bytes = DimsVectorUtils::Count(dims) * data_type_size;
    if (size_in_bytes > buffer_size_) {
        return Status(TNNERR_OPENCL_MEMALLOC_ERROR, "OpenCL buffer is smaller than the need!");
    }
    cl_int ret = CL_SUCCESS;
    auto output_buffer_ptr =
        command_queue->enqueueMapBuffer(*buffer_, true, CL_MAP_READ, 0, buffer_size_, nullptr, nullptr, &ret);
    if (ret != CL_SUCCESS) {
        CHECK_CL_SUCCESS(ret)
        return Status(TNNERR_OPENCL_MEMMAP_ERROR, "OpenCL MemMap failed");
    }
    memcpy(mat.GetData(), output_buffer_ptr, size_in_bytes);
    ret = command_queue->enqueueUnmapMemObject(*buffer_, output_buffer_ptr);
    if (ret != CL_SUCCESS) {
        CHECK_CL_SUCCESS(ret)
        return Status(TNNERR_OPENCL_MEMUNMAP_ERROR, "OpenCL MemUnMap falied");
    }
    return TNN_OK;
}

//enqueueMapBuffer get cpu buffer pointer, copy mat to buffer pointer, enqueueUnmapMemObject.
Status OpenCLMatConverterAcc::CopyMatToBufferData(Mat &mat, cl::CommandQueue *command_queue) {
    MatType mat_type   = mat.GetMatType();
    int data_type_size = 1;
    DimsVector dims    = mat.GetDims();
    if (mat_type == NCHW_FLOAT) {
        data_type_size = sizeof(float);
    } else if (mat_type == N8UC4) {
        //special for 8UC4, blob channel <= 4.
        dims[1] = 4;
    }
    int size_in_bytes = DimsVectorUtils::Count(dims) * data_type_size;
    if (size_in_bytes > buffer_size_) {
        return Status(TNNERR_OPENCL_MEMALLOC_ERROR, "OpenCL buffer is smaller than the need!");
    }
    cl_int ret = CL_SUCCESS;
    auto output_buffer_ptr =
        command_queue->enqueueMapBuffer(*buffer_, true, CL_MAP_WRITE, 0, buffer_size_, nullptr, nullptr, &ret);
    if (ret != CL_SUCCESS) {
        CHECK_CL_SUCCESS(ret)
        return Status(TNNERR_OPENCL_MEMMAP_ERROR, "OpenCL MemMap failed");
    }
    memcpy(output_buffer_ptr, mat.GetData(), size_in_bytes);
    ret = command_queue->enqueueUnmapMemObject(*buffer_, output_buffer_ptr);
    if (ret != CL_SUCCESS) {
        CHECK_CL_SUCCESS(ret)
        return Status(TNNERR_OPENCL_MEMUNMAP_ERROR, "OpenCL MemUnMap falied");
    }
    return TNN_OK;
}

Status OpenCLMatConverterAcc::SetConvertArgs(OpenCLExecuteUnit &unit, Mat &src, Mat &dst,
                                              bool convert_to_mat) {
    MatType mat_type = src.GetMatType();
    auto dims        = dst.GetDims();

    uint32_t idx     = SetExecuteUnit2DSizeInfoDefault(unit, dims);

    cl_int cl_ret;
    if (DEVICE_NAIVE == src.GetDeviceType()) {
        cl::Image *image = static_cast<cl::Image *>(dst.GetData());
        cl_ret = unit.ocl_kernel.setArg(idx++, *image);
        CHECK_CL_SUCCESS(cl_ret);
        cl_ret = unit.ocl_kernel.setArg(idx++, *buffer_);
        CHECK_CL_SUCCESS(cl_ret);
        //height
        cl_ret = unit.ocl_kernel.setArg(idx++, dims[2]); 
        CHECK_CL_SUCCESS(cl_ret);
        //width
        cl_ret = unit.ocl_kernel.setArg(idx++, dims[3]);
        CHECK_CL_SUCCESS(cl_ret);
    } else if (DEVICE_OPENCL == src.GetDeviceType()) {
        cl::Image *mat_image = static_cast<cl::Image *>(src.GetData());
        cl_ret               = unit.ocl_kernel.setArg(idx++, *mat_image);
        CHECK_CL_SUCCESS(cl_ret);
        cl_ret = unit.ocl_kernel.setArg(idx++, *buffer_);
        CHECK_CL_SUCCESS(cl_ret);
        //height
        cl_ret = unit.ocl_kernel.setArg(idx++, dims[2]); 
        CHECK_CL_SUCCESS(cl_ret);
        //width
        cl_ret = unit.ocl_kernel.setArg(idx++, dims[3]);
        CHECK_CL_SUCCESS(cl_ret);
    } else {
        return Status(TNNERR_PARAM_ERR, "convert type not support yet");
    }
    return TNN_OK;
}

Status OpenCLMatConverterAcc::RunConvertUnit(OpenCLExecuteUnit &unit, cl::CommandQueue *command_queue,
                                              bool need_wait) {
    Status ret = RunKernel(unit.ocl_kernel, unit.global_work_size, unit.local_work_size, command_queue, "MatConvert");
    if (need_wait) {
        //sync
        command_queue->finish();
    }
    return ret;
}

Status OpenCLMatConverterAcc::Resize(Mat& src, Mat& dst, ResizeParam param, void* command_queue) {
    Status ret            = TNN_OK;
    if(src.GetDeviceType() != dst.GetDeviceType()) {
        return Status(TNNERR_PARAM_ERR, "convert type not support yet");
    }
    auto cl_command_queue = static_cast<cl::CommandQueue *>(command_queue);
    if (cl_command_queue == nullptr) {
        LOGE("Get OpenCL command queue failed!\n");
        return Status(TNNERR_NULL_PARAM, "Get OpenCL command queue failed!");
    }
    const std::string key = "Resize";
    OpenCLExecuteUnit unit;
    if(execute_map_.count(key) == 0) {
        std::string program_name = "normalize";
        // std::string kernel_name = "image_scaling";
        std::string kernel_name = "image_bilinear";
        ret = CreateExecuteUnit(unit, program_name, kernel_name);
        if(ret != TNN_OK) {
            return ret;
        }
        execute_map_[key] = unit; 
    }

    auto dims        = dst.GetDims();
    uint32_t idx     = SetExecuteUnit2DSizeInfoDefault(unit, dims);
    float w_scale =  ((float)src.GetWidth() / (float)dst.GetWidth());
    float h_scale =  ((float)src.GetHeight() / (float)dst.GetHeight());
    cl_int cl_ret;
    cl::Image *image_input = static_cast<cl::Image *>(src.GetData());
    cl::Image *image_output = static_cast<cl::Image *>(dst.GetData());
    cl_ret = unit.ocl_kernel.setArg(idx++, *image_input);
    CHECK_CL_SUCCESS(cl_ret);
    cl_ret = unit.ocl_kernel.setArg(idx++, *image_output);
    CHECK_CL_SUCCESS(cl_ret);
    //scale_w
    cl_ret = unit.ocl_kernel.setArg(idx++, w_scale); 
    CHECK_CL_SUCCESS(cl_ret);
    //scale_h
    cl_ret = unit.ocl_kernel.setArg(idx++, h_scale);
    CHECK_CL_SUCCESS(cl_ret);
    //src_w
    cl_ret = unit.ocl_kernel.setArg(idx++, src.GetWidth()); 
    CHECK_CL_SUCCESS(cl_ret);
    //src_h
    cl_ret = unit.ocl_kernel.setArg(idx++,src.GetHeight());
    CHECK_CL_SUCCESS(cl_ret);
    ret = RunConvertUnit(unit, cl_command_queue, false);
    if (ret != TNN_OK) {
        return ret;
    }
    return TNN_OK;
}

Status OpenCLMatConverterAcc::Crop(Mat& src, Mat& dst, CropParam param, void* command_queue) {
    Status ret            = TNN_OK;
    if(src.GetDeviceType() != dst.GetDeviceType()) {
        return Status(TNNERR_PARAM_ERR, "convert type not support yet");
    }
    // else if(src.GetDims() != dst.GetDims()) {
    //     return Status(TNNERR_PARAM_ERR, "convert type not support yet");
    // }
    auto cl_command_queue = static_cast<cl::CommandQueue *>(command_queue);
    if (cl_command_queue == nullptr) {
        LOGE("Get OpenCL command queue failed!\n");
        return Status(TNNERR_NULL_PARAM, "Get OpenCL command queue failed!");
    }
    const std::string key = "Crop"; 
    OpenCLExecuteUnit unit;
    if(execute_map_.count(key) == 0) {
        std::string program_name = "copy";
        std::string kernel_name = "Crop";
        ret = CreateExecuteUnit(unit, program_name, kernel_name);
        if(ret != TNN_OK) {
            return ret;
        }
        execute_map_[key] = unit; 
    }

    auto dims        = dst.GetDims();
    uint32_t idx     = SetExecuteUnit2DSizeInfoDefault(unit, dims);

    cl_int cl_ret;
    //int offset = param.top_left_x + param.top_left_y*src.GetWidth();
    cl::Image *image_input = static_cast<cl::Image *>(src.GetData());
    cl::Image *image_output = static_cast<cl::Image *>(dst.GetData());
    cl_ret = unit.ocl_kernel.setArg(idx++, *image_input);
    CHECK_CL_SUCCESS(cl_ret);
    cl_ret = unit.ocl_kernel.setArg(idx++, *image_output);
    CHECK_CL_SUCCESS(cl_ret);
    //start_x
    cl_ret = unit.ocl_kernel.setArg(idx++, param.top_left_x); 
    CHECK_CL_SUCCESS(cl_ret);
    //start_y
    cl_ret = unit.ocl_kernel.setArg(idx++, param.top_left_y);
    CHECK_CL_SUCCESS(cl_ret);
    //width
    cl_ret = unit.ocl_kernel.setArg(idx++, param.width); 
    CHECK_CL_SUCCESS(cl_ret);
    //height
    cl_ret = unit.ocl_kernel.setArg(idx++, param.height);
    CHECK_CL_SUCCESS(cl_ret);

    ret = RunConvertUnit(unit, cl_command_queue, false);
    if (ret != TNN_OK) {
        return ret;
    }
    return TNN_OK;
}

Status OpenCLMatConverterAcc::WarpAffine(Mat& src, Mat& dst, WarpAffineParam param, void* command_queue) {
    Status ret            = TNN_OK;
    auto cl_command_queue = static_cast<cl::CommandQueue *>(command_queue);
    if (cl_command_queue == nullptr) {
        LOGE("Get OpenCL command queue failed!\n");
        return Status(TNNERR_NULL_PARAM, "Get OpenCL command queue failed!");
    } 
    if(execute_map_.count("WarpAffine") == 0) {        

    }
    return ret;
}
DECLARE_MAT_CONVERTER_CREATER(OpenCL);
REGISTER_MAT_CONVERTER(OpenCL, DEVICE_OPENCL);
}  // namespace TNN_NS
