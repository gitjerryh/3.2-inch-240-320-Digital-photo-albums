#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <SPIFFS.h>
#include <TJpg_Decoder.h>
#include <esp_wifi.h>
#include <esp_system.h>

#define WIFI_SSID "ESP32-Album"     
#define WIFI_PASSWORD "12345678"     
#define MAX_CONNECTIONS 4 // 最大连接数
#define WIFI_POWER 20.5  // WiFi发射功率,最大20.5dBm
#define WIFI_CHANNEL 6      // 使用固定信道6
#define BEACON_INTERVAL 100 // 信标间隔100ms
#define TX_POWER 82        // WiFi发射功率(约19.5dBm)

TFT_eSPI tft = TFT_eSPI();
WebServer server(80);

// 屏幕分辨率
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

// 调试标志
#define DEBUG_ANIMATION true

// 动画相关常量
const unsigned long ANIMATION_INTERVAL = 50;  // 动画更新间隔(ms)

// 动画相关变量
unsigned long lastAnimationUpdate = 0;  // 上次动画更新时间
uint8_t animationFrame = 0;            // 动画帧计数器
uint8_t breathBrightness = 0;          // 呼吸效果亮度
bool isUploading = false;              // 上传状态标志

// 修改图片动画相关结构
struct ImageAnimation {
    float wave = 0;             // 波动效果
    float waveStrength = 0;     // 波动强度
    float waveSpeed = 0;        // 波动速度
    unsigned long lastWave = 0;  // 上次波动时间
    unsigned long waveInterval = 0; // 波动间隔
    bool inWave = false;        // 是否在波动中
    float damping = 0.95;       // 阻尼系数
    bool enabled = false;       // 动画启用标志
    
    // 添加变形网格
    static const int GRID_SIZE = 16;  // 网格大小
    float gridX[GRID_SIZE][GRID_SIZE] = {0}; // X方向偏移
    float gridY[GRID_SIZE][GRID_SIZE] = {0}; // Y方向偏移
    float targetX[GRID_SIZE][GRID_SIZE] = {0}; // 目标X偏移
    float targetY[GRID_SIZE][GRID_SIZE] = {0}; // 目标Y偏移
    float springStrength = 0.2;  // 弹簧强度
    float friction = 0.8;        // 摩擦力
} imgAnim;

// 在全局变量区域添加
#define MIN_HEAP_SIZE 30000  // 最小堆内存(bytes)
hw_timer_t *watchDog = NULL; // 看门狗定时器
unsigned long lastHeapCheck = 0; // 上次内存检查时间
const int HEAP_CHECK_INTERVAL = 10000; // 内存检查间隔(ms)
int lowMemCount = 0; // 低内存计数

// 添加显示模式枚举
enum DisplayMode {
    CLEAR_MODE,      // 清晰显示模式
    DYNAMIC_MODE     // 动态油画模式
};

// 添加全局变量
DisplayMode currentDisplayMode = DYNAMIC_MODE;
const char* DISPLAY_MODE_FILE = "/display_mode.txt";  // 用于保存显示模式

// 添加保存和读取显示模式的函数
void saveDisplayMode() {
    File file = SPIFFS.open(DISPLAY_MODE_FILE, FILE_WRITE);
    if(file) {
        file.write((uint8_t)currentDisplayMode);
        file.close();
    }
}

void loadDisplayMode() {
    if(SPIFFS.exists(DISPLAY_MODE_FILE)) {
        File file = SPIFFS.open(DISPLAY_MODE_FILE, FILE_READ);
        if(file) {
            currentDisplayMode = (DisplayMode)file.read();
            file.close();
        }
    }
}

// 修改JPEG解码回调函数
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap)
{
    if(currentDisplayMode == CLEAR_MODE) {
        // 清晰模式直接显示
        tft.pushImage(x, y, w, h, bitmap);
        return true;
    }
    
    // 动态油画模式的现有代码
    if(ESP.getFreeHeap() < MIN_HEAP_SIZE) {
        Serial.println("内存不足,跳过图像处理");
        tft.pushImage(x, y, w, h, bitmap);
        return true;
    }
    
    if(!imgAnim.enabled) {
        // 首次显示图片时初始化网格
        static bool firstDisplay = true;
        if(firstDisplay) {
            firstDisplay = false;
            // 初始化网格位置
            for(int i = 0; i < imgAnim.GRID_SIZE; i++) {
                for(int j = 0; j < imgAnim.GRID_SIZE; j++) {
                    imgAnim.gridX[i][j] = 0;
                    imgAnim.gridY[i][j] = 0;
                    imgAnim.targetX[i][j] = 0;
                    imgAnim.targetY[i][j] = 0;
                }
            }
            imgAnim.enabled = true;
        }
        // 直接显示原图
        tft.pushImage(x, y, w, h, bitmap);
        return true;
    }

    // 创建临时缓冲区
    uint16_t* tempBitmap = new uint16_t[w * h];
    if(!tempBitmap) return false;
    
    // 复制原始图像
    memcpy(tempBitmap, bitmap, w * h * sizeof(uint16_t));
    
    // 计算网格单元大小
    float cellWidth = (float)w / (imgAnim.GRID_SIZE - 1);
    float cellHeight = (float)h / (imgAnim.GRID_SIZE - 1);
    
    // 更新网格变形
    for(int i = 0; i < imgAnim.GRID_SIZE; i++) {
        for(int j = 0; j < imgAnim.GRID_SIZE; j++) {
            // 计算弹簧
            float dx = imgAnim.targetX[i][j] - imgAnim.gridX[i][j];
            float dy = imgAnim.targetY[i][j] - imgAnim.gridY[i][j];
            
            // 应用弹簧力和摩擦力
            imgAnim.gridX[i][j] += dx * imgAnim.springStrength;
            imgAnim.gridY[i][j] += dy * imgAnim.springStrength;
            
            // 应用摩擦力
            imgAnim.gridX[i][j] *= imgAnim.friction;
            imgAnim.gridY[i][j] *= imgAnim.friction;
        }
    }
    
    // 应用网格变形到图像
    for(int py = 0; py < h; py++) {
        for(int px = 0; px < w; px++) {
            // 找到周围的网格点
            int gridX = px / cellWidth;
            int gridY = py / cellHeight;
            float fracX = (px % (int)cellWidth) / cellWidth;
            float fracY = (py % (int)cellHeight) / cellHeight;
            
            // 双线性插值计算偏移
            float offsetX = 0, offsetY = 0;
            for(int i = 0; i < 2; i++) {
                for(int j = 0; j < 2; j++) {
                    float weight = (i ? fracX : (1-fracX)) * (j ? fracY : (1-fracY));
                    offsetX += imgAnim.gridX[gridX+i][gridY+j] * weight;
                    offsetY += imgAnim.gridY[gridX+i][gridY+j] * weight;
                }
            }
            
            // 应用偏移
            int sourceX = px + offsetX;
            int sourceY = py + offsetY;
            
            // 确保在边界
            if(sourceX >= 0 && sourceX < w && sourceY >= 0 && sourceY < h) {
                tempBitmap[py * w + px] = bitmap[sourceY * w + sourceX];
            }
        }
    }
    
    // 显示处理后的图像
    tft.pushImage(x, y, w, h, tempBitmap);
    delete[] tempBitmap;
    
    return true;
}

// 添加触发波动效果的函数
void triggerPhotoWave(uint8_t corner = 255) {
    if(!imgAnim.enabled) return;
    
    // 随机选择一个角落或指定角落
    int startCorner = (corner == 255) ? random(4) : corner;
    
    // 根据选择的角落设置网格目标位置
    for(int i = 0; i < imgAnim.GRID_SIZE; i++) {
        for(int j = 0; j < imgAnim.GRID_SIZE; j++) {
            float distToCorner = 0;
            switch(startCorner) {
                case 0: // 左上
                    distToCorner = sqrt(i*i + j*j);
                    break;
                case 1: // 右上
                    distToCorner = sqrt((imgAnim.GRID_SIZE-1-i)*(imgAnim.GRID_SIZE-1-i) + j*j);
                    break;
                case 2: // 左下
                    distToCorner = sqrt(i*i + (imgAnim.GRID_SIZE-1-j)*(imgAnim.GRID_SIZE-1-j));
                    break;
                case 3: // 右下
                    distToCorner = sqrt((imgAnim.GRID_SIZE-1-i)*(imgAnim.GRID_SIZE-1-i) + 
                                      (imgAnim.GRID_SIZE-1-j)*(imgAnim.GRID_SIZE-1-j));
                    break;
            }
            
            float strength = (1.0 - distToCorner / imgAnim.GRID_SIZE) * 10;
            if(strength > 0) {
                imgAnim.targetX[i][j] = random(-strength, strength);
                imgAnim.targetY[i][j] = random(-strength, strength);
            }
        }
    }
}

void handleRoot() {
    Serial.println("处理根路径请求");
    
    // 设置HTTP响应头的Content-Type和charset
    server.sendHeader("Content-Type", "text/html; charset=utf-8");
    
    String html = "<html><head>";
    // 添加charset meta标签
    html += "<meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    // 现代化的CSS样式
    html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
    html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #f0f2f5; color: #333; }";
    html += ".container { max-width: 800px; margin: 20px auto; padding: 20px; background: white; border-radius: 12px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
    html += "h1 { color: #1a73e8; text-align: center; margin-bottom: 30px; font-size: 2em; }";
    
    // 改进的模式选择样式
    html += ".mode-select { display: flex; justify-content: center; gap: 20px; margin: 25px 0; }";
    html += ".mode-option { position: relative; }";
    html += ".mode-option input { display: none; }";
    html += ".mode-option label { display: block; padding: 10px 20px; background: #f5f5f5; border-radius: 8px; cursor: pointer; transition: all 0.3s; }";
    html += ".mode-option input:checked + label { background: #1a73e8; color: white; }";
    
    // 文件上传区域样式
    html += ".upload-area { border: 2px dashed #ccc; border-radius: 12px; padding: 40px 20px; text-align: center; margin: 20px 0; transition: all 0.3s; }";
    html += ".upload-area.drag-over { border-color: #1a73e8; background: rgba(26,115,232,0.1); }";
    html += ".upload-area i { font-size: 48px; color: #666; margin-bottom: 15px; }";
    
    // 预览容器样式
    html += ".preview-container { max-width: 100%; margin: 20px auto; text-align: center; }";
    html += ".preview { max-width: 100%; max-height: 400px; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1); display: none; }";
    
    // 按钮样式
    html += ".button { background: #1a73e8; color: white; padding: 12px 24px; border: none; border-radius: 8px; cursor: pointer; font-size: 16px; transition: all 0.3s; }";
    html += ".button:hover { background: #1557b0; transform: translateY(-1px); }";
    html += ".button:active { transform: translateY(1px); }";
    html += ".button.disabled { background: #ccc; cursor: not-allowed; }";
    
    // 进度条样式
    html += ".progress-bar { height: 4px; background: #f0f0f0; border-radius: 2px; margin: 20px 0; display: none; }";
    html += ".progress-bar-fill { height: 100%; background: #1a73e8; border-radius: 2px; width: 0%; transition: width 0.3s; }";
    
    // 提示信息样式
    html += ".message { padding: 12px; border-radius: 8px; margin: 10px 0; display: none; }";
    html += ".message.success { background: #e6f4ea; color: #1e8e3e; }";
    html += ".message.error { background: #fce8e6; color: #d93025; }";
    
    html += "</style>";
    
    // 添加字体图标
    html += "<link rel='stylesheet' href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.15.4/css/all.min.css'>";
    html += "<script src='https://cdn.jsdelivr.net/npm/heic2any@0.0.4/dist/heic2any.min.js'></script>";
    html += "</head><body>";
    
    html += "<div class='container'>";
    html += "<h1><i class='fas fa-images'></i> ESP32 照片相册</h1>";
    
    // 改进的模式选择
    html += "<div class='mode-select'>";
    html += "<div class='mode-option'><input type='radio' id='stretch' name='mode' value='stretch' checked><label for='stretch'><i class='fas fa-expand'></i> 拉伸填充</label></div>";
    html += "<div class='mode-option'><input type='radio' id='fit' name='mode' value='fit'><label for='fit'><i class='fas fa-compress'></i> 保持比例</label></div>";
    html += "</div>";
    
    // 在上传区域前添加模式切换按钮
    html += "<div class='mode-switch'>";
    html += "<button id='clearMode' class='mode-btn" + String(currentDisplayMode == CLEAR_MODE ? " active" : "") + 
            "' onclick='switchMode(\"clear\")'><i class='fas fa-image'></i> 清晰模式</button>";
    html += "<button id='dynamicMode' class='mode-btn" + String(currentDisplayMode == DYNAMIC_MODE ? " active" : "") + 
            "' onclick='switchMode(\"dynamic\")'><i class='fas fa-paint-brush'></i> 动态模式</button>";
    html += "</div>";
    
    // 添加模式切换的CSS
    html += "<style>";
    html += ".mode-switch { display: flex; justify-content: center; gap: 10px; margin: 20px 0; }";
    html += ".mode-btn { padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; ";
    html += "background: #f0f0f0; color: #333; transition: all 0.3s; }";
    html += ".mode-btn:hover { background: #e0e0e0; }";
    html += ".mode-btn.active { background: #1a73e8; color: white; }";
    html += "</style>";
    
    // 添加模式切换的JavaScript
    html += "<script>";
    html += "function switchMode(mode) {";
    html += "  fetch('/switch-mode?mode=' + mode)";
    html += "    .then(response => response.text())";
    html += "    .then(result => {";
    html += "      if(result === 'success') {";
    html += "        document.getElementById('clearMode').classList.toggle('active', mode === 'clear');";
    html += "        document.getElementById('dynamicMode').classList.toggle('active', mode === 'dynamic');";
    html += "        showMessage('显示模式已切换', 'success');";
    html += "      }";
    html += "    });";
    html += "}";
    html += "</script>";
    
    // 文件上传区域
    html += "<div class='upload-area' id='dropZone'>";
    html += "<i class='fas fa-cloud-upload-alt'></i>";
    html += "<p>点击或拖拽图片此处上传</p>";
    html += "<input type='file' accept='image/*,.heic' id='fileInput' style='display:none'>";
    html += "</div>";
    
    // 预览区域
    html += "<div class='preview-container'>";
    html += "<img id='preview' class='preview'>";
    html += "</div>";
    
    // 进度条
    html += "<div class='progress-bar' id='progressBar'>";
    html += "<div class='progress-bar-fill' id='progressBarFill'></div>";
    html += "</div>";
    
    // 消息提示
    html += "<div class='message' id='message'></div>";
    
    // 上传按钮
    html += "<button id='uploadBtn' class='button' style='display:none'><i class='fas fa-upload'></i> 上传到显示屏</button>";
    
    html += "</div>";
    
    // JavaScript代码
    html += "<script>";
    html += "let currentFile = null;";
    
    // 拖拽上传支持
    html += "const dropZone = document.getElementById('dropZone');";
    html += "const fileInput = document.getElementById('fileInput');";
    
    html += "dropZone.onclick = () => fileInput.click();";
    
    html += "['dragenter', 'dragover', 'dragleave', 'drop'].forEach(eventName => {";
    html += "  dropZone.addEventListener(eventName, preventDefaults, false);";
    html += "  document.body.addEventListener(eventName, preventDefaults, false);";
    html += "});";
    
    html += "function preventDefaults (e) {";
    html += "  e.preventDefault();";
    html += "  e.stopPropagation();";
    html += "}";
    
    html += "['dragenter', 'dragover'].forEach(eventName => {";
    html += "  dropZone.addEventListener(eventName, highlight, false);";
    html += "});";
    
    html += "['dragleave', 'drop'].forEach(eventName => {";
    html += "  dropZone.addEventListener(eventName, unhighlight, false);";
    html += "});";
    
    html += "function highlight(e) { dropZone.classList.add('drag-over'); }";
    html += "function unhighlight(e) { dropZone.classList.remove('drag-over'); }";
    
    html += "dropZone.addEventListener('drop', handleDrop, false);";
    html += "fileInput.addEventListener('change', handleChange, false);";
    
    // 文件处理函数
    html += "async function handleFile(file) {";
    html += "  if (!file) return;";
    html += "  let processedFile = file;";
    
    // HEIC处理
    html += "  if (file.type === 'image/heic' || file.name.toLowerCase().endsWith('.heic')) {";
    html += "    showMessage('正在转换HEIC格式...', 'info');";
    html += "    try {";
    html += "      const blob = await heic2any({ blob: file, toType: 'image/jpeg' });";
    html += "      processedFile = new File([blob], file.name.replace('.heic', '.jpg'), { type: 'image/jpeg' });";
    html += "      showMessage('HEIC转换成功', 'success');";
    html += "    } catch (error) {";
    html += "      showMessage('HEIC转换失败: ' + error, 'error');";
    html += "      return;";
    html += "    }";
    html += "  }";
    
    html += "  currentFile = processedFile;";
    html += "  const reader = new FileReader();";
    html += "  reader.onload = function(e) {";
    html += "    const img = document.getElementById('preview');";
    html += "    img.src = e.target.result;";
    html += "    img.style.display = 'block';";
    html += "    document.getElementById('uploadBtn').style.display = 'block';";
    html += "  };";
    html += "  reader.readAsDataURL(processedFile);";
    html += "}";
    
    // 处理拖放
    html += "function handleDrop(e) {";
    html += "  const dt = e.dataTransfer;";
    html += "  const file = dt.files[0];";
    html += "  handleFile(file);";
    html += "}";
    
    // 处理文件选择
    html += "function handleChange(e) {";
    html += "  const file = e.target.files[0];";
    html += "  handleFile(file);";
    html += "}";
    
    // 显示消息函数
    html += "function showMessage(text, type) {";
    html += "  const message = document.getElementById('message');";
    html += "  message.textContent = text;";
    html += "  message.className = 'message ' + type;";
    html += "  message.style.display = 'block';";
    html += "  setTimeout(() => message.style.display = 'none', 3000);";
    html += "}";
    
    // 处理上传
    html += "function processAndUpload() {";
    html += "  if (!currentFile) return;";
    html += "  const uploadBtn = document.getElementById('uploadBtn');";
    html += "  uploadBtn.classList.add('disabled');";
    html += "  uploadBtn.disabled = true;";
    
    html += "  const img = document.getElementById('preview');";
    html += "  const canvas = document.createElement('canvas');";
    html += "  const ctx = canvas.getContext('2d');";
    html += "  canvas.width = 240;";
    html += "  canvas.height = 320;";
    
    html += "  const tempImg = new Image();";
    html += "  tempImg.onload = function() {";
    html += "    const mode = document.querySelector('input[name=\"mode\"]:checked').value;";
    html += "    if (mode === 'stretch') {";
    html += "      ctx.drawImage(tempImg, 0, 0, 240, 320);";
    html += "    } else {";
    html += "      const scale = Math.min(240 / tempImg.width, 320 / tempImg.height);";
    html += "      const scaledWidth = tempImg.width * scale;";
    html += "      const scaledHeight = tempImg.height * scale;";
    html += "      const x = (240 - scaledWidth) / 2;";
    html += "      const y = (320 - scaledHeight) / 2;";
    html += "      ctx.fillStyle = 'black';";
    html += "      ctx.fillRect(0, 0, 240, 320);";
    html += "      ctx.drawImage(tempImg, x, y, scaledWidth, scaledHeight);";
    html += "    }";
    
    // 显示上传进度
    html += "    const progressBar = document.getElementById('progressBar');";
    html += "    const progressBarFill = document.getElementById('progressBarFill');";
    html += "    progressBar.style.display = 'block';";
    
    html += "    canvas.toBlob(function(blob) {";
    html += "      const formData = new FormData();";
    html += "      formData.append('photo', blob, 'photo.jpg');";
    
    html += "      const xhr = new XMLHttpRequest();";
    html += "      xhr.upload.onprogress = function(e) {";
    html += "        if (e.lengthComputable) {";
    html += "          const percentComplete = (e.loaded / e.total) * 100;";
    html += "          progressBarFill.style.width = percentComplete + '%';";
    html += "        }";
    html += "      };";
    
    html += "      xhr.onload = function() {";
    html += "        if (xhr.status === 200) {";
    html += "          showMessage('上传成功！', 'success');";
    html += "        } else {";
    html += "          showMessage('上传失败，请重试', 'error');";
    html += "        }";
    html += "        progressBar.style.display = 'none';";
    html += "        progressBarFill.style.width = '0%';";
    html += "        uploadBtn.classList.remove('disabled');";
    html += "        uploadBtn.disabled = false;";
    html += "      };";
    
    html += "      xhr.onerror = function() {";
    html += "        showMessage('上传出错，请检查连接', 'error');";
    html += "        progressBar.style.display = 'none';";
    html += "        progressBarFill.style.width = '0%';";
    html += "        uploadBtn.classList.remove('disabled');";
    html += "        uploadBtn.disabled = false;";
    html += "      };";
    
    html += "      xhr.open('POST', '/upload', true);";
    html += "      xhr.send(formData);";
    html += "    }, 'image/jpeg', 0.95);";
    html += "  };";
    html += "  tempImg.src = img.src;";
    html += "}";
    
    html += "document.getElementById('uploadBtn').onclick = processAndUpload;";
    html += "</script>";
    
    html += "</body></html>";
    server.send(200, "text/html; charset=utf-8", html);
}

void handleUpload() {
    Serial.println("处理上传请求");
    server.send(200, "text/plain", "");
}

// 修改计算缩放比例的函数
uint8_t calculateJpegScale(uint16_t imageWidth, uint16_t imageHeight) {
    // 计算理想缩放比例
    float scaleW = (float)imageWidth / SCREEN_WIDTH;
    float scaleH = (float)imageHeight / SCREEN_HEIGHT;
    
    // 选择合适的缩放比例，确保片完整显示并尽可能大
    float idealScale = (scaleW > scaleH) ? scaleW : scaleH;
    
    // TJpg_Decoder只支持1,2,4,8的缩放
    uint8_t scale = 1;
    if (idealScale <= 1) {
        return 1;  // 图片小于或等于屏幕尺寸
    } else if (idealScale <= 2) {
        return 2;
    } else if (idealScale <= 4) {
        return 4;
    } else {
        return 8;
    }
}

// 添加动画状态枚举
enum CameraAnimState {
    MOVING,         // 正常移动
    TAKING_PHOTO,   // 拍照状态
    SHAKE          // 震动状态
};

// 修改/添加动画相关的全局变量
float cameraX = SCREEN_WIDTH / 2;   // 相机X轴位置
float cameraY = SCREEN_HEIGHT / 2;   // 相机Y轴位置
float cameraRotation = 0;           // ��机旋转角度
float lensRotation = 0;             // 镜头旋转度
CameraAnimState animState = MOVING;  // 动画状态
unsigned long stateStartTime = 0;    // 状态开始时间
float animTime = 0;                 // 动画时间累积器
const float TRAIL_LENGTH = 10;      // 轨迹长度
struct TrailPoint {
    float x, y;
    uint16_t color;
} trailPoints[10];                  // 存储轨迹点

// 修改相机图标绘制函数
void drawCameraIcon(float x, float y, int size, uint16_t color, bool flash) {
    // 保存当前变换状态
    float scale = 1.0;
    if(animState == TAKING_PHOTO) {
        // 拍照时稍微放大
        scale = 1.2;
    } else if(animState == SHAKE) {
        // 震动效果
        scale = 1.0 + sin(millis() * 0.1) * 0.1;
    }
    
    size *= scale;
    
    // 绘制轨迹
    for(int i = 0; i < TRAIL_LENGTH - 1; i++) {
        if(trailPoints[i].color != 0) {
            uint16_t trailColor = tft.color565(
                (trailPoints[i].color >> 11) * (TRAIL_LENGTH - i) / TRAIL_LENGTH,
                ((trailPoints[i].color >> 5) & 0x3F) * (TRAIL_LENGTH - i) / TRAIL_LENGTH,
                (trailPoints[i].color & 0x1F) * (TRAIL_LENGTH - i) / TRAIL_LENGTH
            );
            tft.drawPixel(trailPoints[i].x, trailPoints[i].y, trailColor);
        }
    }

    // 绘制���机背景阴影
    tft.fillRoundRect(x - size/2 + 2, y - size/3 + 2, 
                     size, size*2/3, size/6, tft.color565(30,30,30));
    
    // 相机主体(更圆润的边角)
    tft.fillRoundRect(x - size/2, y - size/3, 
                     size, size*2/3, size/6, color);
                     
    // 添加装饰条纹
    uint16_t stripeColor = tft.color565(
        ((color >> 11) & 0x1F) * 0.7,
        ((color >> 5) & 0x3F) * 0.7,
        (color & 0x1F) * 0.7
    );
    tft.fillRoundRect(x - size/2, y - size/6, 
                     size, size/12, size/24, stripeColor);
    
    // 取景器(更大更突出)
    tft.fillRoundRect(x + size/4, y - size/2, 
                     size/4, size/6, size/16, TFT_BLACK);
    tft.fillRoundRect(x + size/4 + 1, y - size/2 + 1, 
                     size/4 - 2, size/6 - 2, size/16, color);
    
    // 旋转的镜头(添加多层)
    float lensX = x + cos(lensRotation) * size/8;
    float lensY = y + sin(lensRotation) * size/8;
    
    // 镜头外圈
    tft.fillCircle(lensX, lensY, size/3, TFT_BLACK);
    // 镜头主体
    tft.fillCircle(lensX, lensY, size/3 - 2, color);
    // 镜头内圈
    tft.fillCircle(lensX, lensY, size/4, TFT_BLACK);
    tft.fillCircle(lensX, lensY, size/4 - 2, color);
    // 镜头中心
    tft.fillCircle(lensX, lensY, size/6, TFT_BLACK);
    
    // 添加小按钮装饰
    tft.fillCircle(x - size/3, y - size/4, size/12, TFT_BLACK);
    tft.fillCircle(x - size/3, y - size/4, size/12 - 1, stripeColor);
    
    // 闪光灯效果(更大更明显)
    if(flash || animState == TAKING_PHOTO) {
        // 闪光灯光晕效果
        for(int r = size/3; r > 0; r -= 2) {
            uint8_t brightness = r * 8;
            brightness = brightness > 255 ? 255 : brightness;
            uint16_t haloColor = tft.color565(
                brightness,
                brightness,
                brightness
            );
            tft.drawCircle(x + size/3, y - size/2, r, haloColor);
        }
        // 闪光灯主体
        tft.fillRoundRect(x + size/3 - size/12, y - size/2 - size/12,
                         size/6, size/6, size/24, TFT_WHITE);
    } else {
        // 未闪光时的闪光灯
        tft.fillRoundRect(x + size/3 - size/12, y - size/2 - size/12,
                         size/6, size/6, size/24, stripeColor);
    }
    
    // 添加可爱的表情
    // 眨眼效果
    if(animState == TAKING_PHOTO) {
        tft.fillCircle(x - size/6, y - size/6, size/20, TFT_WHITE);
        tft.fillCircle(x + size/6, y - size/6, size/20, TFT_WHITE);
        tft.fillCircle(x - size/6, y - size/6, size/30, TFT_BLACK);
        tft.fillCircle(x + size/6, y - size/6, size/30, TFT_BLACK);
    } else {
        tft.drawLine(x - size/6 - size/30, y - size/6, x - size/6 + size/30, y - size/6, TFT_BLACK);
        tft.drawLine(x + size/6 - size/30, y - size/6, x + size/6 + size/30, y - size/6, TFT_BLACK);
    }
    
    // 添加小耳朵装饰
    tft.fillCircle(x - size/2 + size/12, y - size/3, size/20, TFT_WHITE);
    tft.fillCircle(x + size/2 - size/12, y - size/3, size/20, TFT_WHITE);
}

// 修改待机动画函数
void drawStandbyAnimation() {
    unsigned long currentMillis = millis();
    
    if (currentMillis - lastAnimationUpdate >= ANIMATION_INTERVAL) {
        lastAnimationUpdate = currentMillis;
        animTime += 0.05;  // 动画时间递增
        
        // 清除屏幕
        tft.fillScreen(TFT_BLACK);
        
        // 更新动画状态
        unsigned long stateDuration = currentMillis - stateStartTime;
        switch(animState) {
            case MOVING:
                if(stateDuration > 5000) {  // 每5秒拍一次照
                    animState = TAKING_PHOTO;
                    stateStartTime = currentMillis;
                }
                break;
                
            case TAKING_PHOTO:
                if(stateDuration > 500) {  // 拍照动作持续500ms
                    animState = SHAKE;
                    stateStartTime = currentMillis;
                }
                break;
                
            case SHAKE:
                if(stateDuration > 300) {  // 震动持续300ms
                    animState = MOVING;
                    stateStartTime = currentMillis;
                }
                break;
        }
        
        // 计算相机位置（8字形轨迹）
        float t = animTime;
        float radius = 40;  // 运动半径
        cameraX = SCREEN_WIDTH/2 + radius * sin(t);
        cameraY = SCREEN_HEIGHT/2 - 30 + radius * sin(t * 2) / 2;
        
        // 更新镜头旋转
        lensRotation += 0.1;
        
        // 更新轨迹点
        for(int i = TRAIL_LENGTH-1; i > 0; i--) {
            trailPoints[i] = trailPoints[i-1];
        }
        trailPoints[0] = {cameraX, cameraY, TFT_CYAN};
        
        // 计算相机颜色（呼吸效果）
        breathBrightness = (sin(animTime) + 1) * 127;
        uint16_t iconColor = tft.color565(breathBrightness, breathBrightness, breathBrightness);
        
        // 绘制相机
        drawCameraIcon(cameraX, cameraY, 50, iconColor, 
                      animState == TAKING_PHOTO);
        
        // 添加旋转的星星装饰
        uint16_t starColor = tft.color565(255, 215, 0); // 金色
        float starRadius = 5;
        float starX = cameraX + 40 * cos(animTime * 2);
        float starY = cameraY - 50 + 40 * sin(animTime * 2);
        tft.fillCircle(starX, starY, starRadius, starColor);
        
        // 添加闪烁的光点
        if((int(animTime * 10) % 20) < 10) {
            tft.fillCircle(cameraX, cameraY + 60, 3, TFT_WHITE);
        }
        
        // 添加小动画元素（如旋转的心形）
        uint16_t heartColor = tft.color565(255, 0, 0); // 红色
        float heartX = cameraX - 40 * cos(animTime * 3);
        float heartY = cameraY + 60 + 40 * sin(animTime * 3);
        tft.fillCircle(heartX, heartY, 4, heartColor);
        
        // 绘提示文字
        tft.setTextDatum(MC_DATUM);
        tft.setTextSize(2);
        uint16_t textColor = tft.color565(0, breathBrightness+128, breathBrightness+128);
        tft.setTextColor(textColor);
        
        // 文字跟随相机移动
        String message = "Waiting for Upload";
        if(animState == TAKING_PHOTO) {
            message = "*CLICK*";
        }
        tft.drawString(message, SCREEN_WIDTH/2, SCREEN_HEIGHT/2 + 70);
        
        // 动态箭头
        const char* arrows[] = {"^", "^ ^", "^ ^ ^"};
        tft.setTextSize(3);
        tft.setTextColor(TFT_GREEN);
        tft.drawString(arrows[animationFrame % 3], SCREEN_WIDTH/2, SCREEN_HEIGHT/2 + 110);
        
        // WiFi信息
        tft.setTextSize(1);
        tft.setTextColor(TFT_WHITE);
        tft.drawString("Network: ESP32-Album", SCREEN_WIDTH/2, SCREEN_HEIGHT - 40);
        tft.drawString("Connect to: 192.168.4.1", SCREEN_WIDTH/2, SCREEN_HEIGHT - 20);
        
        animationFrame++;
    }
}

// 修改handleFileUpload函数
void handleFileUpload() {
    HTTPUpload& upload = server.upload();
    static File file;
    
    if(upload.status == UPLOAD_FILE_START) {
        isUploading = true;
        if(DEBUG_ANIMATION) {
            Serial.println("开始上传,停止动画");
        }
        if(SPIFFS.exists("/photo.jpg")) {
            SPIFFS.remove("/photo.jpg");
        }
        file = SPIFFS.open("/photo.jpg", FILE_WRITE);
        if(!file) {
            Serial.println("文件创建失败");
            return server.send(500, "text/plain", "Failed to open file for writing");
        }
    } 
    else if(upload.status == UPLOAD_FILE_WRITE) {
        if(file) {
            file.write(upload.buf, upload.currentSize);
        }
    } 
    else if(upload.status == UPLOAD_FILE_END) {
        if(file) {
            file.close();
            if(DEBUG_ANIMATION) {
                Serial.println("上传完成,显示动态图片");
            }
            isUploading = false;
            
            // 初始化波动动画参数
            imgAnim.wave = 0;
            imgAnim.waveStrength = 0;
            imgAnim.waveSpeed = 0;
            imgAnim.lastWave = millis();
            imgAnim.waveInterval = random(3000, 8000);
            imgAnim.inWave = false;
            imgAnim.enabled = true;
            
            // 显示图片
            tft.fillScreen(TFT_BLACK);
            TJpgDec.drawFsJpg(0, 0, "/photo.jpg");
        }
        server.send(200, "text/plain", "Upload successful");
    }
}

// 在文件开头的全局变量声明部分添加
#define LOADING_BAR_WIDTH 180
#define LOADING_BAR_HEIGHT 12
#define LOADING_BAR_X ((SCREEN_WIDTH - LOADING_BAR_WIDTH) / 2)
#define LOADING_BAR_Y ((SCREEN_HEIGHT - LOADING_BAR_HEIGHT) / 2)

// 改进开机动画函数
void showBootAnimation() {
    tft.fillScreen(TFT_BLACK);
    
    // 绘制初始相机图标
    int iconSize = 40;
    drawCameraIcon(SCREEN_WIDTH/2, SCREEN_HEIGHT/3, iconSize, TFT_WHITE, false);
    
    // 标题文字动画效果
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    
    // 修改标题显示部分
    const char* title = "ESP32 Album";
    // 减小字符间距为16像素
    int charSpacing = 16;  
    
    // 计算整个标题的总宽度
    int totalWidth = strlen(title) * charSpacing;
    // 计算起始X坐标，使标题居中
    int titleX = (SCREEN_WIDTH - totalWidth) / 2;
    int titleY = SCREEN_HEIGHT/3 + 50;
    
    for(int i = 0; title[i] != '\0'; i++) {
        char c[2] = {title[i], '\0'};
        tft.setTextColor(random(0xFFFF)); // 随机颜色
        // 使用对位置来绘制每个字符
        tft.drawString(c, titleX + i*charSpacing, titleY);
        delay(100);
    }
    
    // 调整进度条位置，将其移到屏幕下方
    #undef LOADING_BAR_Y
    #define LOADING_BAR_Y (SCREEN_HEIGHT * 2/3)
    
    // 绘制进度条外框
    tft.drawRoundRect(LOADING_BAR_X - 2, LOADING_BAR_Y - 2,
                     LOADING_BAR_WIDTH + 4, LOADING_BAR_HEIGHT + 4, 
                     LOADING_BAR_HEIGHT/2, TFT_WHITE);
    
    // 进度条动画
    for(int i = 0; i <= 100; i++) {
        // 计算进度条宽度
        int progressWidth = (LOADING_BAR_WIDTH * i) / 100;
        
        // 使用渐变色填充进度条
        uint16_t color = tft.color565(map(i, 0, 100, 0, 255), 
                                    map(i, 0, 100, 0, 128), 
                                    255);
        
        // 清除旧进度条区域
        tft.fillRoundRect(LOADING_BAR_X, LOADING_BAR_Y,
                         LOADING_BAR_WIDTH, LOADING_BAR_HEIGHT,
                         LOADING_BAR_HEIGHT/2, TFT_BLACK);
                         
        // 绘制新进度条
        tft.fillRoundRect(LOADING_BAR_X, LOADING_BAR_Y,
                         progressWidth, LOADING_BAR_HEIGHT,
                         LOADING_BAR_HEIGHT/2, color);
        
        // 显示百分比
        tft.setTextSize(1);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        String percentage = String(i) + "%";
        tft.drawString(percentage, SCREEN_WIDTH/2, 
                      LOADING_BAR_Y - 20);  // 将百分比显示在进度条上方
        
        // 更新相机图标(添加flash参数)
        drawCameraIcon(SCREEN_WIDTH/2, SCREEN_HEIGHT/3, 
                      iconSize, TFT_WHITE, false);
                      
        // 显示动态加载提示
        static const char* messages[] = {
            "Initializing",      // 初始化系统
            "Network Setup",     // 配置网络
            "Getting Ready",     // 准备就绪
            "Welcome"           // 欢迎使用
        };
        int msgIndex = i / 25;
        if(msgIndex > 3) msgIndex = 3;
        
        String msg = String(messages[msgIndex]);
        // 添加动态点号
        for(int j = 0; j <= (i % 3); j++) {
            msg += ".";
        }
        
        // 清除旧消息区域
        tft.fillRect(0, LOADING_BAR_Y + LOADING_BAR_HEIGHT + 10,
                    SCREEN_WIDTH, 20, TFT_BLACK);
                    
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawString(msg, SCREEN_WIDTH/2,
                      LOADING_BAR_Y + LOADING_BAR_HEIGHT + 20);  // 将提示信息显示在进度条下方
        
        delay(20);
    }
    
    // 移除闪烁效果，为平滑过渡
    tft.fillScreen(TFT_BLACK);
    
    // 显示最终欢迎界面
    tft.setTextSize(2);
    tft.setTextColor(TFT_GREEN);
    tft.drawString("Ready!", SCREEN_WIDTH/2, SCREEN_HEIGHT/2);
    delay(1000);
    tft.fillScreen(TFT_BLACK);
}

// 添加模式切换处理函数
void handleSwitchMode() {
    String mode = server.arg("mode");
    if(mode == "clear") {
        currentDisplayMode = CLEAR_MODE;
    } else if(mode == "dynamic") {
        currentDisplayMode = DYNAMIC_MODE;
    }
    
    saveDisplayMode();  // 保存当前模式
    
    // 如果当前有图片显示，重新显示
    if(SPIFFS.exists("/photo.jpg")) {
        tft.fillScreen(TFT_BLACK);
        TJpgDec.drawFsJpg(0, 0, "/photo.jpg");
    }
    
    server.send(200, "text/plain", "success");
}

void setup() {
    Serial.begin(115200);
    
    // WiFi初始化
    Serial.println("正在初始化WiFi...");
    
    // 完全关闭WiFi并重置
    WiFi.persistent(false);  // 禁用WiFi配置持久化
    WiFi.disconnect(true);   // 断开所有连接
    WiFi.mode(WIFI_OFF);     // 关闭WiFi
    delay(100);
    
    esp_wifi_stop();        // 完全停止WiFi
    delay(100);
    
    esp_wifi_deinit();      // 反初始化WiFi
    delay(100);
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // 使用默认配置
    esp_wifi_init(&cfg);    // 重新初始化WiFi
    delay(100);
    
    esp_wifi_start();       // 启动WiFi
    delay(100);
    
    // 设置WiFi模式
    WiFi.mode(WIFI_AP);
    delay(100);
    
    // 配置AP参数
    wifi_config_t conf = {};
    memset(&conf, 0, sizeof(conf));
    
    // 设置SSID和密码
    strcpy((char*)conf.ap.ssid, WIFI_SSID);
    strcpy((char*)conf.ap.password, WIFI_PASSWORD);
    conf.ap.ssid_len = strlen(WIFI_SSID);
    conf.ap.channel = WIFI_CHANNEL;
    conf.ap.authmode = WIFI_AUTH_WPA2_PSK;
    conf.ap.ssid_hidden = 0;
    conf.ap.max_connection = MAX_CONNECTIONS;
    conf.ap.beacon_interval = BEACON_INTERVAL;
    
    // 应用AP配置
    esp_wifi_set_config(WIFI_IF_AP, &conf);
    
    // 设置国家代码
    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = TX_POWER,
        .policy = WIFI_COUNTRY_POLICY_MANUAL
    };
    esp_wifi_set_country(&country);
    
    // 设置协议模式(b/g)
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
    
    // 设置发射功率
    esp_wifi_set_max_tx_power(TX_POWER);
    
    // 配置IP地址
    WiFi.softAPConfig(IPAddress(192,168,4,1),    // AP IP
                     IPAddress(192,168,4,1),    // 网关
                     IPAddress(255,255,255,0)); // 子网掩码
    
    // 启动AP
    bool apSuccess = WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL, false, MAX_CONNECTIONS);
    if (!apSuccess) {
        Serial.println("AP配置失败,重启设备!");
        delay(1000);
        ESP.restart();
        return;
    }
    
    // 等待AP完全启动
    delay(500);
    
    Serial.println("AP模式启动成功");
    Serial.printf("SSID: %s\n", WIFI_SSID);
    Serial.printf("密码: %s\n", WIFI_PASSWORD);
    Serial.printf("信道: %d\n", WIFI_CHANNEL);
    Serial.printf("IP地址: %s\n", WiFi.softAPIP().toString().c_str());
    
    // WiFi事件处理
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
        Serial.print("Station connected, MAC: ");
        uint8_t* mac = info.wifi_ap_staconnected.mac;
        for(int i = 0; i < 6; i++){
            Serial.printf("%02X", mac[i]);
            if(i < 5) Serial.print(":");
        }
        Serial.println();
    }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
    
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
        Serial.print("Station disconnected, MAC: ");
        uint8_t* mac = info.wifi_ap_stadisconnected.mac;
        for(int i = 0; i < 6; i++){
            Serial.printf("%02X", mac[i]);
            if(i < 5) Serial.print(":");
        }
        Serial.println();
    }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

    // 其他初始化代码...
    // 初始化SPIFFS
    if(!SPIFFS.begin(true)) {
        Serial.println("SPIFFS挂载失败");
        return;
    }
    
    // 初始化显示屏
    tft.begin();
    tft.setRotation(2);
    tft.fillScreen(TFT_BLACK);
    
    // 显示开机动画
    showBootAnimation();
    
    // 初始化JPEG解码器
    TJpgDec.setCallback(tft_output);
    TJpgDec.setSwapBytes(true);
    
    // 配置Web服务器路由
    server.on("/", HTTP_GET, handleRoot);
    server.on("/upload", HTTP_POST, handleUpload, handleFileUpload);
    
    // 启动服务器
    server.begin();
    Serial.println("HTTP服务器已启动");
    
    // 确保初始状态
    isUploading = false;
    
    // 删除可能存在的旧图片文件
    if(SPIFFS.exists("/photo.jpg")) {
        SPIFFS.remove("/photo.jpg");
        if(DEBUG_ANIMATION) {
            Serial.println("删除旧图片文件");
        }
    }
    
    // 初始化看门狗定时器(5秒超时)
    watchDog = timerBegin(0, 80, true);
    timerAttachInterrupt(watchDog, []() {
        ESP.restart();  // 超时重启
    }, true);
    timerAlarmWrite(watchDog, 5000000, false);
    timerAlarmEnable(watchDog);
    
    // 加载显示模式
    loadDisplayMode();
    
    // 添加模式切换路由
    server.on("/switch-mode", HTTP_GET, handleSwitchMode);
}

void loop() {
    // 喂狗
    timerWrite(watchDog, 0);
    
    // 定期检查内存
    if(millis() - lastHeapCheck > HEAP_CHECK_INTERVAL) {
        lastHeapCheck = millis();
        
        size_t freeHeap = ESP.getFreeHeap();
        if(freeHeap < MIN_HEAP_SIZE) {
            lowMemCount++;
            Serial.printf("警告:内存不足 %d bytes\n", freeHeap);
            
            if(lowMemCount > 3) {  // 连续3次内存不足
                Serial.println("内存持续不足,准备重启...");
                ESP.restart();
            }
        } else {
            lowMemCount = 0;  // 重置计数
        }
        
        // 打印内存信息
        Serial.printf("空闲堆内存: %d bytes\n", freeHeap);
        Serial.printf("最大空闲块: %d bytes\n", ESP.getMaxAllocHeap());
    }

    // 处理Web服务器请求
    server.handleClient();
    
    // 优先检查是否有图片需要显示
    if (SPIFFS.exists("/photo.jpg") && !isUploading) {
        static unsigned long lastWaveCheck = 0;
        static unsigned long lastRefresh = 0;
        const unsigned long REFRESH_INTERVAL = 60000; // 每分钟强制刷新一次
        
        // 定期完全重绘以防止屏幕残影
        if(millis() - lastRefresh > REFRESH_INTERVAL) {
            lastRefresh = millis();
            tft.fillScreen(TFT_BLACK);
            TJpgDec.drawFsJpg(0, 0, "/photo.jpg");
            return;
        }
        
        // 随机触发波动效果
        if(millis() - lastWaveCheck > random(3000, 8000)) {
            lastWaveCheck = millis();
            
            // 检查内存是否足够进行动画
            if(ESP.getFreeHeap() > MIN_HEAP_SIZE) {
                triggerPhotoWave();
                TJpgDec.drawFsJpg(0, 0, "/photo.jpg");
            } else {
                Serial.println("内存不足,跳过动画效果");
            }
        }
        return;
    }
    
    // 待机动画相关
    static unsigned long lastFrameTime = 0;
    const unsigned long targetFrameTime = 1000 / 20; // 限制最大帧率为20fps
    
    if (!isUploading && !SPIFFS.exists("/photo.jpg")) {
        unsigned long currentTime = millis();
        if(currentTime - lastFrameTime >= targetFrameTime) {
            lastFrameTime = currentTime;
            
            // 检查内存是否足够
            if(ESP.getFreeHeap() > MIN_HEAP_SIZE) {
                drawStandbyAnimation();
            } else {
                // 如果内存不足,显示简单的等待信息
                tft.fillScreen(TFT_BLACK);
                tft.setTextDatum(MC_DATUM);
                tft.setTextColor(TFT_WHITE);
                tft.drawString("Waiting for Upload...", SCREEN_WIDTH/2, SCREEN_HEIGHT/2);
                delay(1000); // 降低刷新频率
            }
        }
    }
}