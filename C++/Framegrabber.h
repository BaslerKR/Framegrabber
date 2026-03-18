#ifndef FRAMEGRABBER_H
#define FRAMEGRABBER_H
#include <atomic>
#include <mutex>
#include <functional>
#include <thread>
#include <cstdint>
#include <iostream>
#include <string>
#include <memory>
#include <algorithm>
#undef Sleep // define to avoid the conflict with the Sleep macro in os_funcs.h

#include "basler_fg.h"
#include "fg_struct.h"
#include "sisoIo.h"
#include "siso_genicam.h"

class Framegrabber
{
public:
    struct Image {
        const uint8_t* data = nullptr;
        size_t size = 0;
        int width = 0;
        int height = 0;
        int bytesPerPixel = 0;
        int frameId = 0;
        int pixelFormat = 0;
    };

    Framegrabber(std::string serialNumber, unsigned int boardIndex);
    ~Framegrabber();
    enum Status{
        GrabbingStatus,
        ConnectionStatus
    };
    using StatusCallback = std::function<void(Status status, bool on)>;
    void onGrabberStatus(StatusCallback cb){
        _scb = std::move(cb);
    }

    using GrabCallback = std::function<void(unsigned int dmaIndex, const Image& image)>;
    void onGrabbed(GrabCallback cb) {
        std::lock_guard<std::mutex> lk(_cbMutex);
        _gcb = std::move(cb);
    }

    bool loadApplet(std::string hapPath);
    bool loadMCF(std::string mcfPath);
    void unload();

    void setDMABufferSize(size_t num) {
        std::lock_guard<std::mutex> lk(_mutex);
        _dmaBufferCount = std::max<size_t>(1, num);
    }

    bool getParameter(int paramID, const int dmaIndex, void *value, bool silence=false);
    bool getParameter(const std::string& paramName, const int dmaIndex, void* value, bool silence=false);

    FgParamTypes getParameterProperty(int param, const int dmaIndex);
    FgParamTypes getParameterProperty(std::string param, const int dmaIndex);

    bool setParameter(int param, const int dmaIndex, void* value, bool silence=false);
    bool setParameter(const std::string& paramName, const int dmaIndex, void* value, bool silence=false);


    std::string getSerialNumber();
    int getDMACount();
    int getWidth(const int dmaIndex);
    int getHeight(int dmaIndex);
    int getX(int dmaIndex);
    int getY(int dmaIndex);
    int getBytesPerPixel(int dmaIndex);
    Fg_Struct* getFg(){ return _this; }

    void grab(unsigned int dmaIndex, size_t frameCnt=0);
    void grabAll(size_t frameCnt=0);
    void stop(unsigned int dmaIndex);
    void stopAll();

    bool saveConfig(std::string path);

    /* Camera Control */
    bool initCXPModule();
    void updateCXPCameraList();
    SgcCameraHandle *getCameraFromDMA(int dmaIndex);
    bool openCamera(SgcCameraHandle* camera);
    void closeCamera(SgcCameraHandle* camera);
    bool grabCamera(SgcCameraHandle* camera);
    void stopCamera(SgcCameraHandle* camera);


private:
    StatusCallback _scb;
    GrabCallback _gcb;
    std::mutex _cbMutex;
    Fg_Struct *_this = nullptr;
    std::string _serialNumber;
    std::string _boardName;
    unsigned int _boardIndex = 0;
    size_t _dmaBufferCount = 4;


    struct DMAHandler {
        int index = -1;
        dma_mem* mem = nullptr;
        std::thread th;

        std::atomic<bool> stop{ false };
        std::atomic<bool> running{ false };
        std::atomic<bool> acqStarted{ false };
        std::atomic<bool> cameraStarted{ false };
    };

    std::vector<std::unique_ptr<DMAHandler>> _DMAList;
    std::mutex _mutex;

    SgcBoardHandle* _cxpBoard = nullptr;
    std::vector<std::pair<unsigned int, SgcCameraHandle*>> _cxpCameras;

};

#endif // FRAMEGRABBER_H








