#include "fancontrol.h"
#include "tmp451.h"

//Fan curve table
const TemperaturePoint defaultTable[] =
{
    { .temperature_c = 20,  .fanLevel_f = 0.1   },
    { .temperature_c = 40,  .fanLevel_f = 0.5   },
    { .temperature_c = 50,  .fanLevel_f = 0.6   },
    { .temperature_c = 60,  .fanLevel_f = 0.7   },
    { .temperature_c = 100, .fanLevel_f = 1     }
};

TemperaturePoint *fanControllerTable;

//Fan control state
Thread FanControllerThread;
volatile bool fanControllerThreadExit = false;
volatile bool systemInSleepMode = false;
volatile bool thermalEmergency = false;

//Adaptive timing
static u64 currentSleepTime = 30000000000ULL; // Start with 30 seconds
static float lastTemperature = 0;
static float lastFanLevel = 0;
static u32 stableReadings = 0;

//Power state monitoring
Event powerStateEvent;
bool powerStateEventInitialized = false;

//Thermal thresholds for emergency response
#define EMERGENCY_TEMP_THRESHOLD 80.0f
#define CRITICAL_TEMP_THRESHOLD 90.0f
#define TEMP_CHANGE_THRESHOLD 2.0f
#define STABLE_READINGS_FOR_SLOWDOWN 10

//Sleep intervals (nanoseconds)
#define MIN_SLEEP_INTERVAL    1000000000ULL   // 1 second (emergency)
#define NORMAL_SLEEP_INTERVAL 10000000000ULL  // 10 seconds (active)
#define LONG_SLEEP_INTERVAL   30000000000ULL  // 30 seconds (stable)
#define SLEEP_MODE_INTERVAL   300000000000ULL // 5 minutes (sleep mode)

//Log
char logPath[PATH_MAX];

void CreateDir(char *dir)
{
    if (!dir) return;
    
    char dirPath[PATH_MAX] = {0};
    char *pos = dir;
    char *next;
    
    while ((next = strchr(pos, '/')) != NULL) {
        size_t len = next - dir;
        if (len > 0 && len < PATH_MAX - 1) {
            strncpy(dirPath, dir, len);
            dirPath[len] = '\0';
            
            if (access(dirPath, F_OK) == -1) {
                mkdir(dirPath, 0755); // More restrictive permissions
            }
        }
        pos = next + 1;
    }
    
    // Handle final directory
    if (strlen(dir) < PATH_MAX && access(dir, F_OK) == -1) {
        mkdir(dir, 0755);
    }
}

void InitLog()
{
    if(access(LOG_DIR, F_OK) == -1)
        CreateDir(LOG_DIR);

    // Don't remove existing log, just truncate on first write
    FILE *log = fopen(LOG_FILE, "w");
    if (log) {
        fprintf(log, "Fan Controller Started - Ultra Optimized Version\n");
        fclose(log);
    }
}

void WriteLog(const char *buffer)
{
    if (!buffer) return;
    
    FILE *log = fopen(LOG_FILE, "a");
    if(log != NULL)
    {
        time_t now;
        time(&now);
        struct tm *timeinfo = localtime(&now);
        
        fprintf(log, "[%02d:%02d:%02d] %s\n", 
                timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, buffer);
        fclose(log);
    }
}

void WriteConfigFile(TemperaturePoint *table)
{
    TemperaturePoint *tableToWrite = table;
    
    if(tableToWrite == NULL)
    {
        tableToWrite = malloc(sizeof(defaultTable));
        if (!tableToWrite) return;
        memcpy(tableToWrite, defaultTable, sizeof(defaultTable));
    }

    if(access(CONFIG_DIR, F_OK) == -1)
        CreateDir(CONFIG_DIR);

    FILE *config = fopen(CONFIG_FILE, "wb");
    if (config) {
        fwrite(tableToWrite, sizeof(defaultTable), 1, config);
        fclose(config);
    }
    
    if (table == NULL) {
        free(tableToWrite);
    }
}

void ReadConfigFile(TemperaturePoint **table_out)
{
    if (!table_out) return;
    
    InitLog();

    *table_out = malloc(sizeof(defaultTable));
    if (!*table_out) {
        WriteLog("Memory allocation failed");
        return;
    }
    
    memcpy(*table_out, defaultTable, sizeof(defaultTable));

    if(access(CONFIG_DIR, F_OK) == -1)
    {
        CreateDir(CONFIG_DIR);
        WriteConfigFile(NULL);
        WriteLog("Created missing config directory");
    }
    else
    {
        if(access(CONFIG_FILE, F_OK) == -1)
        {
            WriteConfigFile(NULL);
            WriteLog("Created missing config file");
        }
        else
        {
            FILE *config = fopen(CONFIG_FILE, "rb");
            if (config) {
                size_t bytesRead = fread(*table_out, sizeof(defaultTable), 1, config);
                fclose(config);
                
                if (bytesRead == 1) {
                    WriteLog("Config file loaded successfully");
                } else {
                    WriteLog("Config file corrupted, using defaults");
                    memcpy(*table_out, defaultTable, sizeof(defaultTable));
                }
            }
        }
    }    
}

// Power state monitoring functions
void InitPowerStateMonitoring()
{
    // Initialize power state event monitoring if available
    Result rs = eventCreate(&powerStateEvent, true);
    if (R_SUCCEEDED(rs)) {
        powerStateEventInitialized = true;
        WriteLog("Power state monitoring initialized");
    } else {
        WriteLog("Power state monitoring unavailable");
    }
}

bool CheckSystemSleepState()
{
    // This would need to be implemented based on the specific system API
    // For now, we'll use a placeholder that assumes active state
    // In a real implementation, this would check system power management state
    
    // Placeholder logic - in real implementation, check actual system state
    static u32 checkCounter = 0;
    checkCounter++;
    
    // Simulate sleep detection based on system activity
    // This is where you'd integrate with actual system power management APIs
    return false; // For now, assume always active
}

float CalculateFanLevel(float temperatureC_f)
{
    if (!fanControllerTable) return 0.0f;
    
    // Handle edge cases
    if (temperatureC_f <= 0) return 0.0f;
    if (temperatureC_f <= fanControllerTable->temperature_c) {
        // Linear interpolation from 0 to first point
        float m = fanControllerTable->fanLevel_f / fanControllerTable->temperature_c;
        return m * temperatureC_f;
    }
    
    // Check if above maximum temperature
    if (temperatureC_f >= (fanControllerTable + 4)->temperature_c) {
        return (fanControllerTable + 4)->fanLevel_f;
    }
    
    // Find the correct temperature range and interpolate
    for(int i = 0; i < (TABLE_SIZE/sizeof(TemperaturePoint)) - 1; i++)
    {
        TemperaturePoint *current = fanControllerTable + i;
        TemperaturePoint *next = fanControllerTable + i + 1;
        
        if(temperatureC_f >= current->temperature_c && temperatureC_f <= next->temperature_c)
        {
            float tempDiff = next->temperature_c - current->temperature_c;
            if (tempDiff <= 0) return current->fanLevel_f; // Avoid division by zero
            
            float m = (next->fanLevel_f - current->fanLevel_f) / tempDiff;
            float q = current->fanLevel_f - (m * current->temperature_c);
            
            return (m * temperatureC_f) + q;
        }
    }
    
    return 0.0f; // Fallback
}

u64 CalculateAdaptiveSleepTime(float currentTemp, float fanLevel)
{
    // Emergency response for high temperatures
    if (currentTemp >= CRITICAL_TEMP_THRESHOLD) {
        thermalEmergency = true;
        return MIN_SLEEP_INTERVAL; // 1 second
    }
    
    if (currentTemp >= EMERGENCY_TEMP_THRESHOLD) {
        thermalEmergency = true;
        return MIN_SLEEP_INTERVAL * 2; // 2 seconds
    }
    
    thermalEmergency = false;
    
    // If in sleep mode, use very long intervals
    if (systemInSleepMode) {
        return SLEEP_MODE_INTERVAL; // 5 minutes
    }
    
    // Check temperature stability
    float tempChange = fabs(currentTemp - lastTemperature);
    float fanChange = fabs(fanLevel - lastFanLevel);
    
    if (tempChange < TEMP_CHANGE_THRESHOLD && fanChange < 0.05f) {
        stableReadings++;
    } else {
        stableReadings = 0;
    }
    
    // Adaptive timing based on stability
    if (stableReadings >= STABLE_READINGS_FOR_SLOWDOWN) {
        return LONG_SLEEP_INTERVAL; // 30 seconds when stable
    } else if (tempChange < TEMP_CHANGE_THRESHOLD * 2) {
        return NORMAL_SLEEP_INTERVAL; // 10 seconds for small changes
    } else {
        return NORMAL_SLEEP_INTERVAL / 2; // 5 seconds for rapid changes
    }
}

void FanControllerThreadFunction(void* arg)
{
    (void)arg; // Suppress unused parameter warning
    
    FanController fc;
    float fanLevelSet_f = 0;
    float temperatureC_f = 0;
    char logBuffer[256];

    Result rs = fanOpenController(&fc, 0x3D000001);
    if(R_FAILED(rs))
    {
        WriteLog("ERROR: Failed to open fan controller");
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
        return;
    }

    WriteLog("Fan controller thread started");
    
    // Initialize power state monitoring
    InitPowerStateMonitoring();

    while(!fanControllerThreadExit)
    {
        // Check system sleep state
        systemInSleepMode = CheckSystemSleepState();
        
        // Get current temperature
        rs = Tmp451GetSocTemp(&temperatureC_f);
        if(R_FAILED(rs))
        {
            WriteLog("ERROR: Failed to get temperature");
            // Don't abort on temperature read failure, try again later
            svcSleepThread(NORMAL_SLEEP_INTERVAL);
            continue;
        }

        // Calculate required fan level
        fanLevelSet_f = CalculateFanLevel(temperatureC_f);
        
        // Only update fan if there's a significant change or it's been a while
        bool shouldUpdateFan = false;
        
        if (thermalEmergency) {
            shouldUpdateFan = true; // Always update in emergency
        } else if (fabs(fanLevelSet_f - lastFanLevel) > 0.02f) {
            shouldUpdateFan = true; // Update if fan level changed significantly
        } else if (systemInSleepMode && fanLevelSet_f > 0.1f) {
            shouldUpdateFan = true; // Ensure fan runs if needed during sleep
        }
        
        if (shouldUpdateFan) {
            rs = fanControllerSetRotationSpeedLevel(&fc, fanLevelSet_f);
            if(R_FAILED(rs))
            {
                WriteLog("ERROR: Failed to set fan speed");
                // Continue operation even if fan control fails
            } else {
                snprintf(logBuffer, sizeof(logBuffer), 
                        "Temp: %.1f°C, Fan: %.1f%%, Sleep: %s", 
                        temperatureC_f, fanLevelSet_f * 100.0f,
                        systemInSleepMode ? "Yes" : "No");
                WriteLog(logBuffer);
            }
        }
        
        // Calculate adaptive sleep time
        currentSleepTime = CalculateAdaptiveSleepTime(temperatureC_f, fanLevelSet_f);
        
        // Store current values for next iteration
        lastTemperature = temperatureC_f;
        lastFanLevel = fanLevelSet_f;
        
        // Sleep for calculated duration
        svcSleepThread(currentSleepTime);
    }

    // Cleanup
    if (powerStateEventInitialized) {
        eventClose(&powerStateEvent);
    }
    
    fanControllerClose(&fc);
    WriteLog("Fan controller thread stopped");
}

void InitFanController(TemperaturePoint *table)
{
    if (!table) {
        WriteLog("ERROR: Invalid fan control table");
        return;
    }
    
    fanControllerTable = table;

    // Reset state variables
    fanControllerThreadExit = false;
    systemInSleepMode = false;
    thermalEmergency = false;
    currentSleepTime = LONG_SLEEP_INTERVAL;
    lastTemperature = 0;
    lastFanLevel = 0;
    stableReadings = 0;

    Result rs = threadCreate(&FanControllerThread, FanControllerThreadFunction, NULL, NULL, 0x4000, 0x3F, -2);
    if(R_FAILED(rs))
    {
        WriteLog("ERROR: Failed to create fan controller thread");
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
    } else {
        WriteLog("Fan controller thread created successfully");
    }
}

void StartFanControllerThread()
{
    Result rs = threadStart(&FanControllerThread);
    if(R_FAILED(rs))
    {
        WriteLog("ERROR: Failed to start fan controller thread");
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
    } else {
        WriteLog("Fan controller thread started successfully");
    }
}

void CloseFanControllerThread()
{   
    WriteLog("Shutting down fan controller thread...");
    
    fanControllerThreadExit = true;
    
    Result rs = threadWaitForExit(&FanControllerThread);
    if(R_FAILED(rs))
    {
        WriteLog("ERROR: Failed to wait for thread exit");
        // Continue with cleanup anyway
    }
    
    threadClose(&FanControllerThread);
    
    // Reset state
    fanControllerThreadExit = false;
    systemInSleepMode = false;
    thermalEmergency = false;
    
    // Free memory
    if (fanControllerTable) {
        free(fanControllerTable);
        fanControllerTable = NULL;
    }
    
    WriteLog("Fan controller shutdown complete");
}

void WaitFanController()
{
    Result rs = threadWaitForExit(&FanControllerThread);
    if(R_FAILED(rs))
    {
        WriteLog("ERROR: Failed to wait for fan controller thread");
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
    }
}
