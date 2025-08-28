/*
 * libhydrium/libhydrium.h
 * 
 * This is the main libhydrium API entry point.
 */

#ifndef HYDRIUM_H_
#define HYDRIUM_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Major version, following semantic versioning.
 * If positive, dictates the API and ABI.
 */
#define HYDRIUM_VERSION_MAJOR 0
/*
 * Minor version, following semantic versioning.
 */
#define HYDRIUM_VERSION_MINOR 5
/*
 * Point release, following semantic versioning.
 */
#define HYDRIUM_VERSION_POINT 0

/*
 * Compute the numeric version. Used to compare version numbers directly
 */
#define HYDRIUM_COMPUTE_VERSION(ma, mi, po) (UINT64_C(0x1000000000) \
    | ((uint64_t)(ma) << 24) \
    | ((uint64_t)(mi) << 12) \
    | ((uint64_t)(po)))

/*
 * The format of the version integer is:
 * 0x1XXXYYYZZZ where XXX is the major version, YYY is the minor version,
 * and ZZZ is the point release.
 */
#define HYDRIUM_VERSION_INT HYDRIUM_COMPUTE_VERSION(\
    HYDRIUM_VERSION_MAJOR, \
    HYDRIUM_VERSION_MINOR, \
    HYDRIUM_VERSION_POINT)

#define HYD_STRINGIFY(n) HYD_STRINGIFY0(n)
#define HYD_STRINGIFY0(n) #n

#define HYDRIUM_VERSION_STRING \
        HYD_STRINGIFY(HYDRIUM_VERSION_MAJOR) "." \
        HYD_STRINGIFY(HYDRIUM_VERSION_MINOR) "." \
        HYD_STRINGIFY(HYDRIUM_VERSION_POINT)

#ifdef _WIN32
    #ifdef HYDRIUM_INTERNAL_BUILD
        #define HYDRIUM_EXPORT __declspec(dllexport)
    #else
        #define HYDRIUM_EXPORT __declspec(dllimport)
    #endif
#else
    #if defined(__GNUC__) || defined(__CLANG__)
        #define HYDRIUM_EXPORT __attribute__((visibility ("default")))
    #else
        #define HYDRIUM_EXPORT
    #endif /* __GNUC__ || __clang__ */
#endif

typedef enum HYDStatusCode {
    /**
     * Everything is OK.
     */
    HYD_OK = 0,
    /**
     * Used internally only, and never returned by the API.
     */
    HYD_DEFAULT = -1,
    /**
     * Another output buffer is needed.
     */
    HYD_NEED_MORE_OUTPUT = -2,
    /**
     * More input must be provided.
     */
    HYD_NEED_MORE_INPUT = -3,
    /**
     * Every proper error code is less than HYD_ERROR_START, which is negative.
     * This is not an actual error and it is never returned.
     */
    HYD_ERROR_START = -10,
    /**
     * A heap allocation failed due to insufficient memory remaining.
     */
    HYD_NOMEM = -13,
    /**
     * Incorrect API use detected.
     */
    HYD_API_ERROR = -14,
    /**
     * An internal error occurred. If this is returned, something went wrong.
     */
    HYD_INTERNAL_ERROR = -15,
} HYDStatusCode;

typedef enum HYDSampleFormat {
    HYD_UINT8,
    HYD_UINT16,
    HYD_FLOAT32,
} HYDSampleFormat;

typedef struct HYDImageMetadata {
    /**
     * The width of the image, in pixels.
     */
    size_t width;

    /**
     * The height of the image, in pixels.
     */
    size_t height;

    /**
     * A flag indicating whether the input data is in Linear Light. If true, the input
     * is treated as linear-light. If false, the input is treated as sRGB.
     * In both cases the input is assumed to have BT.709 primaries (same as sRGB),
     * and a D65 white point.
     */
    int linear_light;

    /**
     * Indicates the horizontal size of a tile. Valid values are
     * 0, 1, 2, and 3, corresponding to 256, 512, 1024, and 2048
     * pixels wide.
     *
     * Tiles may be larger than the image width, in which case only
     * one tile will exist in the horizontal direction.
     *
     * A special value of -1 may be passed, which tells libhydrium to
     * use one Frame for the entire image. This mode uses more memory,
     * but it decodes faster with libjxl.
     */
    int tile_size_shift_x;

    /**
     * Indicates the vertical size of a tile. Valid values are
     * 0, 1, 2, and 3, corresponding to 256, 512, 1024, and 2048
     * pixels high.
     *
     * Tiles may be larger than the image height, in which case only
     * one tile will exist in the vertical direction.
     *
     * A special value of -1 may be passed, which tells libhydrium to
     * use one Frame for the entire image. This mode uses more memory,
     * but it decodes faster with libjxl.
     */
    int tile_size_shift_y;
} HYDImageMetadata;

/* opaque structure */
typedef struct HYDEncoder HYDEncoder;

/**
 * @brief Allocate and return a fresh HYDEncoder struct.
 *
 * @return A new (HYDEncoder *) object upon success, NULL if the allocation fails.
 */
HYDRIUM_EXPORT HYDEncoder *hyd_encoder_new(void);

/**
 * @brief Deallocate and free all resources associated with the given encoder.
 *
 * @param encoder A HYDEncoder struct.
 * @return HYD_OK upon success, or a negative error code upon failure.
 */
HYDRIUM_EXPORT HYDStatusCode hyd_encoder_destroy(HYDEncoder *encoder);

/**
 * @brief Populate the given encoder with the image metadata set to encode.
 * This must be called before hyd_send_tile or its variants.
 *
 * @param encoder A HYDEncoder struct.
 * @param metadata A struct containing various image metadata parameters.
 * @return HYD_OK upon success, a negative error code upon failure.
 */
HYDRIUM_EXPORT HYDStatusCode hyd_set_metadata(HYDEncoder *encoder, const HYDImageMetadata *metadata);

/**
 * @brief Provide an output buffer into which the encoded JPEG XL image will be written.
 *
 * @param encoder A HYDEncoder struct.
 * @param buffer A buffer of bytes that will contain (a part of) the encoded image data.
 * @param buffer_len The length of the buffer, in bytes.
 * @return HYD_OK upon success, a negative error code upon failure.
 */
HYDRIUM_EXPORT HYDStatusCode hyd_provide_output_buffer(HYDEncoder *encoder, uint8_t *buffer, size_t buffer_len);

/**
 * @brief Provide a buffer of RGB pixel data to the hydrium encoder for it to encode.
 *
 * By default, hydrium encodes one tile at a time, and no tile references any other tile. A tile is 256x256,
 * unless tile_size_shift_x and/or tile_size_shift_y are set to something positive. In that case, the size of a
 * tile will be WxH, where W is (256 << tile_size_shift_x) and H is (256 << tile_size_shift_y).
 *
 * This function accepts an array of three buffers of pixel data, although they may overlap.
 *
 * The X and Y coordinates of the tile passed refer to the coordinates in numbers of tiles,
 * not in numbers of pixels, and starts from the upper left and proceeds in raster order.
 * For example, the tile immediately to the right of the upper-left tile has x=1 and y=0.
 *
 * The buffer passed treats buffer[0] as the red channel, buffer[1] as the green channel,
 * and buffer[2] as the blue channel. In the case where these buffers are all distinct, you probably
 * wish to set pixel_stride to 1, as each sample will be next to the previous sample. row_stride
 * is the pointer difference between two rows of a buffer.
 *
 * If you wish to pass packed data instead of planar data, set buffer[0] to the first red sample,
 * set buffer[1] to the first green sample, and set buffer[2] to the first blue sample. The distance
 * between adjacent red samples will be 3 in this case, so set pixel_stride to 3, not to 1. Row-stride
 * is the pointer difference between two rows of this buffer, as before, so buffer[0] + row_stride will
 * point to the first red sample in the second row of the tile.
 *
 * Note that the stride arguments are in units of samples, not bytes. Do not double them for 16-bit input, as
 * C already handles that when you add pointers.
 *
 * If the encoded tile does not fit in the remaining provided output buffer, HYD_NEED_MORE_OUTPUT is returned.
 * If so, you must call hyd_release_output_buffer to regain ownership of the output buffer. Do whatever with the
 * data (fwrite, send to a network stream, whatever), and then you must provide another output buffer with
 * hyd_provide_output_buffer. After you provide another output buffer, do not send the same tile again, instead
 * call hyd_flush to flush the remaining encoded tile to the output buffer.
 *
 * Tiles are always multiples of 256x256, although images are not always a multiple of 256 pixels in each direction.
 * If so, the rightmost column and bottom-most row of tiles will be smaller. Make sure you set row_stride correctly
 * for the right-most column, as hydrium will not automatically assume it is smaller than the one provided. Because
 * the image width and height were provided with hyd_set_metadata, hydrium will know how big these tiles should be
 * and it will not overrun the buffers.
 *
 * If the encoded tile is fully written to the output buffer, then HYD_OK will be returned, and it
 * is time to send another tile. Tiles may be sent in any order, although libhydrium will assume that the
 * lower-right tile is the last tile. Passing 0 or 1 to is_last will explicitly tell libhydrium if the sent tile is
 * the last one - otherwise, pass a negative value to use the above default behavior. Sending another tile
 * after the last one is undefined behavior.
 *
 * Any tile except the last one may be left unsent, and these gaps will be populated by zeroes.
 *
 * If the last tile is sent and the output buffer is not full, HYD_OK is returned. Any return value other than
 * HYD_NEED_MORE_OUTPUT or HYD_OK is an error.
 *
 * The sample format may vary from tile to tile. Internally, libhydrium will scale uint8_t samples, assumed to be
 * full-range between 0 and 255, to float values between 0.0 and 1.0. uint16_t samples will be scaled from the full
 * range of 0 to 65535, to float values between 0.0 and 1.0. To pass out-of-gamut colors you must pass float samples
 * less than 0.0 or more than 1.0, as float samples when passed are not rescaled. However, it is undefined behavior
 * to pass infinite or NaN float samples.
 *
 * @param encoder A HYDEncoder struct.
 * @param buffer An array of three buffers of pixel data.
 * @param tile_x The X-coordinate, in tiles, of the encoded tile.
 * @param tile_y The Y-coordinate, in tiles, of the encoded tile.
 * @param row_stride The line size of the pixel buffer.
 * @param pixel_stride The inter-pixel stride of the buffer.
 * @param is_last A flag indicating whether the sent tile is the last one.
 * @param sample_fmt The sample format of the provided buffer.
 */
HYDRIUM_EXPORT HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const void *const buffer[3],
                                           uint32_t tile_x, uint32_t tile_y, ptrdiff_t row_stride,
                                           ptrdiff_t pixel_stride, int is_last, HYDSampleFormat sample_fmt);

/**
 * @brief Release the output buffer that was previously provided by hyd_provide_output_buffer.
 *
 * The value of *written will be populated by the amount of data actually written to the provided
 * output buffer, which may be smaller than its size. Once this is called, another buffer must
 * be provided with hyd_provide_output_buffer before any more data can be written.
 *
 * @param encoder A HYDEncoder struct.
 * @param written Populated by the amount of data actually written to the buffer.
 * @return HYD_OK upon success, a negative status code upon failure.
 */
HYDRIUM_EXPORT HYDStatusCode hyd_release_output_buffer(HYDEncoder *encoder, size_t *written);

/**
 * @brief Flush the encoded data to the provided buffer.
 *
 * After swapping out a buffer with hyd_release_output_buffer and then again calling
 * hyd_provide_output_buffer, the remaining encoded data in the tile must be drained to the new buffer
 * by calling hyd_flush. Tiles will only be sent once, so this function is used to flush the remaining
 * output to the newly provided buffer.
 *
 * @param encoder A HYDEncoder struct.
 * @return HYD_OK upon success, a negative status code upon failure.
 */
HYDRIUM_EXPORT HYDStatusCode hyd_flush(HYDEncoder *encoder);

/**
 * @brief Returns a string description of the last error that occurred.
 *
 * @param encoder A HYDEncoder struct.
 */
HYDRIUM_EXPORT const char *hyd_error_message_get(HYDEncoder *encoder);

/**
 * @brief Tag the output file with the suggested ICC profile.
 *
 * JPEG XL files can contain ICC profiles for XYB-encoded images, which are
 * suggestions for the input file, rather than descriptions of the pixel data.
 *
 * libhydrium only accepts sRGB and linear sRGB input, but this function lets a
 * client tag the result with an ICC profile, in case CMS was performed before
 * calling libhydrium from a specific ICC profile.
 *
 * @param encoder A HYDEncoder struct
 * @param icc_data A buffer containing an ICC Profile. The buffer is not taken
 *     by the caller, and it can be freed as soon as this function returns.
 * @param icc_size The size of the ICC Profile buffer.
 * @return HYD_OK upon success, a negative error code upon failure.
 */
HYDRIUM_EXPORT HYDStatusCode hyd_set_suggested_icc_profile(HYDEncoder *encoder,
    const uint8_t *icc_data, size_t icc_size);

#endif /* HYDRIUM_H_ */
