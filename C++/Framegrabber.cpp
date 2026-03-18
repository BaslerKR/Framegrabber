#include "Framegrabber.h"
#include "FramegrabberSystem.h"

#include <filesystem>
#include <string>
#include <vector>

Framegrabber::Framegrabber(string serialNumber, unsigned int boardIndex) :
    _serialNumber(serialNumber), _boardIndex(boardIndex){}

Framegrabber::~Framegrabber(){
    {
        std::lock_guard<std::mutex> lk(_mutex);
        for (auto& d : _DMAList) {
            if (d) d->stop.store(true, std::memory_order_relaxed);
        }
    }
    for (unsigned int i = 0; i < _DMAList.size(); ++i) stop(i);
    unload();

}

bool Framegrabber::loadApplet(string hapPath){
    _this = Fg_Init(hapPath.c_str(), _boardIndex);
    if(_this){
        std::filesystem::path path = hapPath;
        syslog(std::string("The applet(") + path.filename().string() + std::string(") successfully loaded."));

        return true;
    }else{
        syslog("Failed to load the applet: " + std::string(Fg_getLastErrorDescription(_this)), true);
    }
    return false;
}

bool Framegrabber::loadMCF(string mcfPath)
{
    if(Fg_InitConfig(mcfPath.c_str(), _boardIndex)){
        std::filesystem::path path = mcfPath;
        syslog(std::string("The configuration file(") + path.filename().string() + ") successfully loaded.");
        return true;
    }else{
        syslog("Failed to load the configuration file: " + std::string(Fg_getLastErrorDescription(_this)), true);
    }
    return false;
}

void Framegrabber::unload()
{
    if (!_this) return;

    stopAll();
    if (_cxpBoard) {
        Sgc_freeBoard(_cxpBoard);
        _cxpBoard = nullptr;
    }

    Fg_FreeGrabber(_this);
    _this = nullptr;
    syslog(std::string("The applet of the grabber ("+ _serialNumber + ") is unloaded."));
}

bool Framegrabber::getParameter(int paramID, int dmaIndex, void* value, bool silence)
{
    if (!_this) {
        if(!silence) syslog("Grabber is not initialized.", true);
        return false;
    }
    if (!value) {
        if(!silence) syslog("Output buffer is null.", true);
        return false;
    }

    const char* paramNameC = Fg_getParameterNameById(_this, paramID, dmaIndex);
    const std::string paramName = paramNameC ? paramNameC : "<unknown>";

    const enum FgParamTypes type = Fg_getParameterTypeById(_this, paramID, dmaIndex);
    if (type == FG_PARAM_TYPE_INVALID) {
        if(!silence) syslog("Invalid parameter type for " + paramName, true);
        return false;
    }

    int rc = FG_OK;

    switch (type) {
    case FG_PARAM_TYPE_INT32_T: {
        int32_t temp = 0;
        rc = Fg_getParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            *static_cast<int32_t*>(value) = temp;
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Read " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_UINT32_T: {
        uint32_t temp = 0;
        rc = Fg_getParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            *static_cast<uint32_t*>(value) = temp;
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Read " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_INT64_T: {
        int64_t temp = 0;
        rc = Fg_getParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            *static_cast<int64_t*>(value) = temp;
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Read " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_UINT64_T: {
        uint64_t temp = 0;
        rc = Fg_getParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            *static_cast<uint64_t*>(value) = temp;
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Read " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_SIZE_T: {
        size_t temp = 0;
        rc = Fg_getParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            *static_cast<size_t*>(value) = temp;
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Read " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_DOUBLE: {
        double temp = 0.0;
        rc = Fg_getParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            *static_cast<double*>(value) = temp;
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Read " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_CHAR_PTR: {
        char temp[1000];

        rc = Fg_getParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            *static_cast<std::string*>(value) = temp;
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Read " + paramName + ": " + (temp ? temp : "<null>"));
        }
        break;
    }

    default:
        if(!silence) syslog("Unsupported parameter type for " + paramName, true);
        return false;
    }

    if (rc != FG_OK) {
        if(!silence) syslog("Failed to get information of " + paramName + ": " + std::string(Fg_getLastErrorDescription(_this)), true);
        return false;
    }

    return true;
}

bool Framegrabber::getParameter(const std::string &paramName, const int dmaIndex, void *value, bool silence)
{
    if (!_this) {
        if(!silence) syslog("Grabber is not initialized.", true);
        return false;
    }

    if (paramName.empty()) {
        if(!silence) syslog("Parameter name is empty.", true);
        return false;
    }

    const int paramID = Fg_getParameterIdByName(_this, paramName.c_str());
    if (paramID < 0) {
        if(!silence) syslog("Failed to resolve parameter name: " + paramName, true);
        return false;
    }

    return getParameter(paramID, dmaIndex, value, silence);
}


FgParamTypes Framegrabber::getParameterProperty(int param, const int dmaIndex)
{
    auto paramName = Fg_getParameterNameById(_this, param, dmaIndex);

    char value[10000];
    int bufLen = sizeof(value);

    auto result = Fg_getParameterProperty(_this, param, FgProperty::PROP_ID_DATATYPE, &value, &bufLen);
    if(result != FG_OK){
        return FG_PARAM_TYPE_INVALID;
    }
    int dataType = atoi(value);
    return FgParamTypes(dataType);
}

FgParamTypes Framegrabber::getParameterProperty(std::string param, const int dmaIndex)
{
    auto paramID = Fg_getParameterIdByName(_this, param.c_str());
    char value[10000];
    int bufLen = sizeof(value);

    auto result = Fg_getParameterProperty(_this, paramID, FgProperty::PROP_ID_DATATYPE, &value, &bufLen);
    if(result != FG_OK){
        return FG_PARAM_TYPE_INVALID;
    }
    int dataType = atoi(value);
    return FgParamTypes(dataType);
}

bool Framegrabber::setParameter(int paramID, const int dmaIndex, void* value, bool silence)
{
    if (!_this) {
        if(!silence) syslog("Grabber is not initialized.", true);
        return false;
    }

    if (!value) {
        if(!silence) syslog("Input value is null.", true);
        return false;
    }

    const char* paramNameC = Fg_getParameterNameById(_this, paramID, dmaIndex);
    const std::string paramName = paramNameC ? paramNameC : "<unknown>";
    const enum FgParamTypes type = Fg_getParameterTypeById(_this, paramID, dmaIndex);
    if (type == FG_PARAM_TYPE_INVALID) {
        if(!silence) syslog("Invalid parameter type for " + paramName, true);
        return false;
    }

    int rc = FG_OK;

    switch (type) {
    case FG_PARAM_TYPE_INT32_T: {
        const int32_t temp = *static_cast<const int32_t*>(value);
        rc = Fg_setParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Set " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_UINT32_T: {
        const uint32_t temp = *static_cast<const uint32_t*>(value);
        rc = Fg_setParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Set " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_INT64_T: {
        const int64_t temp = *static_cast<const int64_t*>(value);
        rc = Fg_setParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Set " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_UINT64_T: {
        const uint64_t temp = *static_cast<const uint64_t*>(value);
        rc = Fg_setParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Set " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_SIZE_T: {
        const size_t temp = *static_cast<const size_t*>(value);
        rc = Fg_setParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Set " + paramName + ": " + std::to_string(temp));
        }
        break;
    }

    case FG_PARAM_TYPE_DOUBLE: {
        const double temp = *static_cast<const double*>(value);
        rc = Fg_setParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Set " + paramName + ": " + std::to_string(temp));
        }
        break;
    }
    case FG_PARAM_TYPE_CHAR_PTR:{
        const std::string temp = *static_cast<const std::string*>(value);
        rc = Fg_setParameterWithType(_this, paramID, &temp, dmaIndex, type);
        if (rc == FG_OK) {
            if(!silence) syslog("DMA(" + std::to_string(dmaIndex) + ") - Set " + paramName + ": " + temp);
        }
        break;
    }
    case FG_PARAM_TYPE_CHAR_PTR_PTR:
    case FG_PARAM_TYPE_STRUCT_FIELDPARAMACCESS:
    case FG_PARAM_TYPE_STRUCT_FIELDPARAMINT:
    case FG_PARAM_TYPE_STRUCT_FIELDPARAMINT64:
    case FG_PARAM_TYPE_STRUCT_FIELDPARAMDOUBLE:
    case FG_PARAM_TYPE_COMPLEX_DATATYPE:
    case FG_PARAM_TYPE_AUTO:
    default:
        if(!silence) syslog("Unsupported parameter type for set: " + paramName, true);
        return false;
    }

    if (rc != FG_OK) {
        const char* err = Fg_getLastErrorDescription(_this);
        if(!silence) syslog("Failed to set " + paramName + (err ? (": " + std::string(err)) : ""), true);
        return false;
    }

    return true;
}

bool Framegrabber::setParameter(const std::string& paramName, const int dmaIndex, void* value, bool silence)
{
    if (!_this) {
        if(!silence) syslog("Grabber is not initialized.", true);
        return false;
    }

    if (paramName.empty()) {
        if(!silence) syslog("Parameter name is empty.", true);
        return false;
    }

    int paramID = Fg_getParameterIdByName(_this, paramName.c_str());
    if (paramID < 0) {
        if(!silence) syslog("Failed to resolve parameter name: " + paramName, true);
        return false;
    }

    return setParameter(paramID, dmaIndex, value, silence);
}

int Framegrabber::getDMACount(){
    if (!_this) return 0;
    int val=0;
    Fg_getParameterPropertyWithType(_this, FG_NR_OF_DMAS, FgProperty::PROP_ID_VALUE, &val);
    return val;
}

int Framegrabber::getWidth(const int dmaIndex){
    int value=-1;
    getParameter(FG_WIDTH, dmaIndex, &value);
    return value;
}

int Framegrabber::getHeight(int dmaIndex){
    int value=-1;
    getParameter(FG_HEIGHT, dmaIndex, &value);
    return value;
}

int Framegrabber::getX(int dmaIndex){
    int value=-1;
    getParameter(FG_XOFFSET, dmaIndex, &value);
    return value;
}

int Framegrabber::getY(int dmaIndex){
    int value=-1;
    getParameter(FG_YOFFSET, dmaIndex, &value);
    return value;
}

int Framegrabber::getBytesPerPixel(int dmaIndex){
    int bytesPerPixel=0;
    int value=-1;
    getParameter(FG_FORMAT, dmaIndex, &value);

    switch (value) {
    case FG_GRAY:
        bytesPerPixel = 1;
        break;
    case FG_GRAY16:
        bytesPerPixel = 2;
        break;
    case FG_COL24:
        bytesPerPixel = 3;
        break;
    case FG_COL32:
        bytesPerPixel = 4;
        break;
    case FG_COL30:
        bytesPerPixel = 5;
        break;
    case FG_COL48:
        bytesPerPixel = 6;
        break;
    }
    return bytesPerPixel;
}

string Framegrabber::getSerialNumber(){
    return _serialNumber;
}

void Framegrabber::grab(unsigned int dmaIndex, size_t frameCnt)
{
    if (_this == nullptr) return;
    {
        std::lock_guard<std::mutex> lk(_mutex);

        if (_DMAList.size() <= dmaIndex)
            _DMAList.resize(dmaIndex + 1);

        if (!_DMAList[dmaIndex]) {
            _DMAList[dmaIndex] = std::make_unique<DMAHandler>();
            _DMAList[dmaIndex]->index = (int)dmaIndex;
        }
    }
    stop(dmaIndex);

    DMAHandler* h = nullptr;
    {
        std::lock_guard<std::mutex> lk(_mutex);
        h = _DMAList[dmaIndex].get();
    }
    if (!h) return;

    const int width = getWidth(static_cast<int>(dmaIndex));
    const int height = getHeight(static_cast<int>(dmaIndex));
    int pixelFormat = -1;
    getParameter(FG_FORMAT, static_cast<int>(dmaIndex), &pixelFormat);
    const int bytesPerPixel = getBytesPerPixel(static_cast<int>(dmaIndex));

    if (width <= 0 || height <= 0 || bytesPerPixel <= 0) {
        syslog("Invalid image size or format.", true);
        return;
    }

    const size_t frameBytes =
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        static_cast<size_t>(bytesPerPixel);
    const size_t totalBufferBytes = frameBytes * _dmaBufferCount;


    dma_mem* mem = Fg_AllocMemEx(_this, totalBufferBytes, static_cast<frameindex_t>(_dmaBufferCount));
    if (!mem) {
        syslog("DMA(" + std::to_string(dmaIndex) + ") - Fg_AllocMemEx failed. ", true);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(_mutex);
        h->mem = mem;
        h->stop.store(false, std::memory_order_relaxed);
        h->running.store(true, std::memory_order_relaxed);        
        h->acqStarted.store(false, std::memory_order_relaxed);
        h->cameraStarted.store(false, std::memory_order_relaxed);
    }

    auto dmaBufferCount = _dmaBufferCount;
    h->th = std::thread([this, dmaIndex, frameCnt, h, width, height, bytesPerPixel, pixelFormat, frameBytes, dmaBufferCount]{
        dma_mem* localMem = nullptr;
        {
            std::lock_guard<std::mutex> lk(_mutex);
            localMem = h->mem;
        }
        if (!localMem) {
            h->running.store(false, std::memory_order_relaxed);
            return;
        }

        const int acqRc = Fg_AcquireEx(_this, static_cast<int>(dmaIndex), GRAB_INFINITE, ACQ_STANDARD, localMem);
        if (acqRc != FG_OK) {
            syslog("DMA(" + std::to_string(dmaIndex) + ") - Fg_AcquireEx failed. " + std::to_string(acqRc) + " " + Fg_getErrorDescription(nullptr, acqRc), true);

            {
                std::lock_guard<std::mutex> lk(_mutex);
                if (h->mem) {
                    Fg_FreeMemEx(_this, h->mem);
                    h->mem = nullptr;
                }
                h->running.store(false, std::memory_order_relaxed);
                h->stop.store(false, std::memory_order_relaxed);
            }
            return;
        }
        h->acqStarted.store(true, std::memory_order_relaxed);

        SgcCameraHandle* cam = nullptr;
        if(_cxpCameras.size() > 0){
            cam = getCameraFromDMA(dmaIndex);
            if (!openCamera(cam)) {
                syslog("DMA(" + std::to_string(dmaIndex) + ") - Failed to connect camera. ", true);
                Fg_stopAcquireEx(_this, static_cast<int>(dmaIndex), localMem, 0);
                {
                    std::lock_guard<std::mutex> lk(_mutex);
                    if (h->mem) {
                        Fg_FreeMemEx(_this, h->mem);
                        h->mem = nullptr;
                    }
                    h->running.store(false, std::memory_order_relaxed);
                    h->stop.store(false, std::memory_order_relaxed);
                }
                return;
            }

            if (!grabCamera(cam)) {
                syslog("DMA(" + std::to_string(dmaIndex) + ") - Failed to start camera acquisition.", true);
                Fg_stopAcquireEx(_this, static_cast<int>(dmaIndex), localMem, 0);

                {
                    std::lock_guard<std::mutex> lk(_mutex);
                    if (h->mem) {
                        Fg_FreeMemEx(_this, h->mem);
                        h->mem = nullptr;
                    }
                    h->running.store(false, std::memory_order_relaxed);
                    h->stop.store(false, std::memory_order_relaxed);
                }
                return;
            }
            h->cameraStarted.store(true, std::memory_order_relaxed);
        }


        syslog("DMA(" + std::to_string(dmaIndex) + ") - " + ((frameCnt != 0) ? ("Expected frame(s):" + std::to_string(frameCnt)): "Continuous")+
               " grabbing started.");

        int lastPic = 0;
        size_t grabbed = 0;

        while (!h->stop.load(std::memory_order_relaxed)) {
            const int rc = Fg_getLastPicNumberBlockingEx(_this, lastPic + 1, static_cast<int>(dmaIndex), 100, localMem);
            if (rc <= 0) {
                if (h->stop.load(std::memory_order_relaxed)) break;
                if (rc == FG_NO_PICTURE_AVAILABLE) continue;

                syslog(std::string("Fg_getLastPicNumberBlockingEx failed. rc=") + std::to_string(rc) + " " + Fg_getErrorDescription(nullptr, rc), true);
                continue;
            }

            lastPic = rc;
            const uint8_t* src = reinterpret_cast<const uint8_t*>(Fg_getImagePtrEx(_this, lastPic, static_cast<int>(dmaIndex), localMem));

            if (!src) {
                syslog("Fg_getImagePtrEx returned null.", true);
                continue;
            }

            Image image;
            image.data = src;
            image.size = frameBytes;
            image.width = width;
            image.height = height;
            image.bytesPerPixel = bytesPerPixel;
            image.pixelFormat = pixelFormat;
            image.frameId = lastPic;

            GrabCallback cbCopy;
            {
                std::lock_guard<std::mutex> lk(_cbMutex);
                cbCopy = _gcb;
            }

            if (cbCopy) {
                try {
                    cbCopy(dmaIndex, image);
                } catch (const std::exception& e) {
                    syslog(std::string("Grab callback threw exception: ") + e.what(), true);
                } catch (...) {
                    syslog("Grab callback threw unknown exception.", true);
                }
            }

            ++grabbed;
            if (frameCnt != 0 && grabbed >= frameCnt) break;
        }


        if (cam && h->cameraStarted.exchange(false, std::memory_order_relaxed)){
            stopCamera(cam);
            closeCamera(cam);
        }
        if (h->acqStarted.exchange(false, std::memory_order_relaxed)) Fg_stopAcquireEx(_this, static_cast<int>(dmaIndex), localMem, 0);

        {
            std::lock_guard<std::mutex> lk(_mutex);
            if (h->mem) {
                Fg_FreeMemEx(_this, h->mem);
                h->mem = nullptr;
            }
            h->running.store(false, std::memory_order_relaxed);
            h->stop.store(false, std::memory_order_relaxed);
        }

        syslog("DMA(" + std::to_string(dmaIndex) + ") - Finished grabbing. Acquired: " + std::to_string(grabbed) + " frame(s)");
    });
}

void Framegrabber::grabAll(size_t frameCnt)
{
    for(int i=0; i<getDMACount(); ++i) grab(i, frameCnt);
}

void Framegrabber::stop(unsigned int dmaIndex)
{
    DMAHandler* h = nullptr;
    dma_mem* mem = nullptr;
    bool acqStarted = false;

    {
        std::lock_guard<std::mutex> lk(_mutex);

        if (_DMAList.size() <= dmaIndex || !_DMAList[dmaIndex]) {
            return;
        }

        h = _DMAList[dmaIndex].get();
        h->stop.store(true, std::memory_order_relaxed);

        mem = h->mem;
        acqStarted = h->acqStarted.load(std::memory_order_relaxed);
    }

    if(_this && mem && acqStarted){
        const int rc = Fg_stopAcquireEx(_this, static_cast<int>(dmaIndex), mem, 0);
        if (rc != FG_OK) {
            syslog("DMA(" + std::to_string(dmaIndex) + ") - stop wake-up rc=" + std::to_string(rc) + " " + Fg_getErrorDescription(nullptr, rc));
        }
    }
    if (h && h->th.joinable() && h->th.get_id() != std::this_thread::get_id()) {
        h->th.join();
    }

}

void Framegrabber::stopAll()
{
    for(int i=0; i<getDMACount(); ++i) stop(i);
}

bool Framegrabber::saveConfig(std::string path)
{
    return FG_OK == Fg_saveConfig(_this, path.c_str());
}

bool Framegrabber::initCXPModule()
{
    if (!_this) {
        syslog("Applet is not loaded.", true);
        return false;
    }

    if(SGC_OK != Sgc_initBoard(_this, 0, &_cxpBoard)){
        syslog("Failed to load the CXP module.", true);
        _cxpBoard = nullptr;
        return false;
    }
    return true;
}

void Framegrabber::updateCXPCameraList()
{
    if(SGC_OK != Sgc_scanPorts(_cxpBoard, 0xFF, 10000, LINK_SPEED_NONE)){
        syslog("Failed to find a camera", true);
        return;
    }
    int cnt = Sgc_getCameraCount(_cxpBoard);
    if (cnt <= 0) {
        syslog("No camera detected.", true);
        return;
    }
    syslog(to_string(cnt) + " Camera(s) detected.");

    _cxpCameras.clear();
    _cxpCameras.reserve(cnt);

    for(int i=0; i < cnt; ++i){
        SgcCameraHandle *cam;
        Sgc_getCameraByIndex(_cxpBoard, i, &cam);
        auto info = Sgc_getCameraInfo(cam);

        unsigned int propertyType = SGC_PROPERTY_TYPE_UINT;
        unsigned int dma = 0xFF;
        Sgc_getCameraPropertyWithType(cam, CAM_PROP_APPLETOPERATORINDEX, &dma, &propertyType, nullptr);

        _cxpCameras.emplace_back(std::pair<unsigned int, SgcCameraHandle*>(dma, cam));
        syslog(std::string("-- DMA(") + to_string(dma) + ") " + info->deviceVendorName + " " + info->deviceModelName + " (" + info->deviceSerialNumber +")");
    }

}

SgcCameraHandle *Framegrabber::getCameraFromDMA(int dmaIndex)
{
    for(const auto cam : _cxpCameras){
        if(cam.first == dmaIndex){
            auto info = Sgc_getCameraInfo(cam.second);
            syslog(std::string("DMA(") + to_string(dmaIndex) + ") - Found the matched " + info->deviceModelName + " ("+ info->deviceSerialNumber + ")");
            return cam.second;
        }
    }
    syslog(std::string("DMA(") + to_string(dmaIndex) + ") Failed to find the matched camera.", true);
    return nullptr;
}


bool Framegrabber::openCamera(SgcCameraHandle *camera)
{
    int index = -1;
    std::string camInfo = "";
    for(const auto cam: _cxpCameras){
        if(cam.second == camera){
            index = cam.first;
            auto info = Sgc_getCameraInfo(cam.second);
            camInfo = std::string() + info->deviceModelName + " (" + info->deviceSerialNumber +")";
        }
    }
    if (SGC_OK == Sgc_connectCamera(camera)){
        syslog("DMA(" + to_string(index) + ") - Connected " + camInfo);
        return true;
    }else{
        syslog("Failed to connect the camera." , true);
        return false;
    }
}

void Framegrabber::closeCamera(SgcCameraHandle *camera)
{
    Sgc_disconnectCamera(camera);
}

bool Framegrabber::grabCamera(SgcCameraHandle *camera)
{
    return SGC_OK == Sgc_startAcquisition(camera, 1);
}

void Framegrabber::stopCamera(SgcCameraHandle *camera)
{
    Sgc_stopAcquisition(camera, 1);
}









