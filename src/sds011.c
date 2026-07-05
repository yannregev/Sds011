#include "sds011.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <termios.h>
#include "includes.h"

static const uint16_t MEASUREMENT_CYCLE_MS = 60000; // 60 seconds

typedef struct {
    int fd;
    uint8_t connected;
    uint8_t bufferIndex;
    uint8_t currentMeasurement;
    uint8_t buffer[10];
    char* port;

} Sds011_t;
static Sds011_t m;


static uint8_t Sds011_Checksum(const uint8_t* data, size_t length);
static void ClearFdBuffer(uint16_t size, const void *data);
static void Sds011_StartMeasurmentTask(uint16_t size, const void *data);
static void SendSleepCommand(uint16_t size, const void *data);
static void SetupUartParameters(void);
static void SendWakeUp(void);
static void SendSleepCommand(uint16_t size, const void *data);
static void ReadData(uint16_t size, const void * data);
static void SetupSds011Parameters(uint16_t size, const void *data);

static uint8_t DataOk(const uint8_t* data, size_t length)
{
    if (length < 10) {
        fprintf(stderr, "Received data length %zu is less than 10 bytes.\n", length);
        return 0;
    }
    uint8_t expectedChecksum = Sds011_Checksum(data, length);
    if (expectedChecksum != data[8]) {
        fprintf(stderr, "Checksum mismatch: expected 0x%02X, got 0x%02X\n", expectedChecksum, data[8]); 
        return 0;
    }
    if (data[length - 1] != 0xAB) {
        fprintf(stderr, "Invalid end byte: 0x%02X\n", data[length - 1]);
        return 0;
    }   
    
    return 1;
}

static void AttemptToConnect(uint16_t size, const void * data) 
{
    m.fd = open(m.port, O_RDWR);
    if (m.fd < 0) {
        return;
    }
    printf("Connected to sds011 on %s\n", m.port);
    m.connected = 1;
    SetupUartParameters();

    SendWakeUp();
    AddPeriodicFunction(ReadData, 100);
    AddDelayedFunction(SetupSds011Parameters, 2000); // Wait 2 seconds before setup
    if (MEASUREMENT_CYCLE_MS >= 40000) {  
        AddDelayedFunction(SendSleepCommand, 1000);
    }

    RemovePeriodicFunction(AttemptToConnect);
}

static void HandleCommunationFailure(void) 
{
    m.connected = 0;
    close(m.fd);
    m.fd = 0;
    fprintf(stderr, "Communication with sds011 failed\n");
    AddPeriodicFunction(AttemptToConnect, 1000);
    RemovePeriodicFunction(ReadData);
    RemovePeriodicFunction(Sds011_StartMeasurmentTask);
}

static uint8_t ReadFromDevice(void) 
{
    size_t bytesRead = read(m.fd, m.buffer, sizeof(m.buffer) - m.bufferIndex);
    if (bytesRead < 0) {
        HandleCommunationFailure();
        return -1;
    }
    return bytesRead;
}

static void WriteToDevice(uint8_t *data, ssize_t size) 
{
    if (write(m.fd, data, size) < 0) {
        HandleCommunationFailure();
        return;
    }
    tcdrain(m.fd);
}

static uint8_t IsDataAck(const uint8_t* data, size_t length)
{
    return data[0] == 0xAA && data[1] == 0xC5;
}

static uint8_t DataIsMeasurment(const uint8_t* data, size_t length)
{
    return data[0] == 0xAA && data[1] == 0xC0;
}

static uint8_t IsDataReadyRead(void)
{
    struct pollfd input;
    input.fd = m.fd;
    input.events = POLLIN;
    int ready = poll(&input, 1, 0);
    if (ready == -1) {
        return 0;
    }
    if (input.revents & POLLIN) {
        return 1;
    }
    return 0;
}

static void SetQueryMode(void)
{
    const unsigned char query_mode_cmd[] = {
        0xAA, 0xB4, 0x02, 0x01, 0x01, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0xFF, 0xFF, 0x02, 0xAB
    };

    write(m.fd, query_mode_cmd, sizeof(query_mode_cmd));
    tcdrain(m.fd);
}

static void QueryDataCommand(uint16_t size, const void *data)
{
    const unsigned char query_data_cmd[] = {
        0xAA, 0xB4, 0x04, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0xFF, 0xFF, 0x02, 0xAB
    };

    write(m.fd, query_data_cmd, sizeof(query_data_cmd));
    tcdrain(m.fd);

}

static void ReadData(uint16_t size, const void *data)
{
    if (!IsDataReadyRead()) {
        return;
    }
    ssize_t bytesRead = ReadFromDevice();
    if (m.bufferIndex + bytesRead < 10) {
        m.bufferIndex += bytesRead;
        return; // Not enough data yet
    }
    m.bufferIndex = 0; // Reset for next read
    if (!DataOk(m.buffer, bytesRead)) {
        ClearFdBuffer(0, NULL);
        return;
    }

    if (DataIsMeasurment(m.buffer, bytesRead)) {
        uint16_t pm25 = m.buffer[2] + (m.buffer[3] << 8);
        uint16_t pm10 = m.buffer[4] + (m.buffer[5] << 8);
        

        EventActivate(EV_AIR_QUALITY_MEASURED, sizeof(EV_AIR_QUALITY_MEASURED_t), &(EV_AIR_QUALITY_MEASURED_t){ 
            .pm10 = pm10,
            .pm25 = pm25,
        });
        return;
    }
    
    if (IsDataAck(m.buffer, bytesRead)) {
        printf("Received ACK from sds011\n");
        return;
    }
    printf("Unknown data received from sds011\n");
    for (size_t i = 0; i < bytesRead; i++) {
        printf("0x%02X ", m.buffer[i]);
    }
    printf("\n");
    ClearFdBuffer(0, NULL);
}

static void ClearFdBuffer(uint16_t size, const void *data)
{
    uint8_t c;
    while (IsDataReadyRead()) {
        read(m.fd, &c, 1);
    }
}

static void SendSleepCommand(uint16_t size, const void *data)
{
    printf("Sending sleep command to sds011\n");
    const unsigned char sleep_cmd[] = {
        0xAA, 0xB4, 0x06, 0x01, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0xFF, 0xFF, 0x05, 0xAB
    };

    write(m.fd, sleep_cmd, sizeof(sleep_cmd));
    tcdrain(m.fd);
}

static void SendWakeUp(void)
{
    printf("Sending wake up command to sds011\n");
    const unsigned char wake_cmd[] = {
        0xAA, 0xB4, 0x06, 0x01, 0x01, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0xFF, 0xFF, 0x06, 0xAB
    };
    write(m.fd, wake_cmd, sizeof(wake_cmd));
    tcdrain(m.fd);
}

static inline uint8_t Sds011_Checksum(const uint8_t* data, size_t length)
{
    int16_t sum = 0;

    for (size_t i = 2; i < length - 2; i++) {
        sum += data[i];
    }
    return (sum & 0xFF);
}

static void Sds011_StartMeasurmentTask(uint16_t size, const void *data)
{
    m.currentMeasurement = 0;
    if (MEASUREMENT_CYCLE_MS < 40000) {
        AddPeriodicFunction(QueryDataCommand, MEASUREMENT_CYCLE_MS); 
    } else {
        SendWakeUp();
        AddDelayedFunction(QueryDataCommand, 30000); 
        AddDelayedFunction(SendSleepCommand, 31000);
        AddDelayedFunction(Sds011_StartMeasurmentTask, MEASUREMENT_CYCLE_MS);
    }
}

static void SetupSds011Parameters(uint16_t size, const void *data)
{
    ClearFdBuffer(0, NULL);
    SetQueryMode();
    QueueFunctionCallback(Sds011_StartMeasurmentTask, 0 , NULL);
}

static void SetupUartParameters(void)
{
    struct termios tty;
    if (tcgetattr(m.fd, &tty) != 0) {
        perror("tcgetattr");
        exit(1);
    }
    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    tty.c_cflag |= (CLOCAL | CREAD);    // ignore modem controls
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;         // 8-bit characters
    tty.c_cflag &= ~PARENB;     // no parity bit
    tty.c_cflag &= ~CSTOPB;     // only need 1 stop bit
    tty.c_cflag &= ~CRTSCTS;    // no hardware flowcontrol
    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // raw mode
    tty.c_oflag &= ~OPOST; // raw output
    if (tcsetattr(m.fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        exit(1);
    }
}

void Sds011_Init(char* portname)
{
    memset(&m, 0, sizeof(Sds011_t));
    m.port = portname;
    AddPeriodicFunction(AttemptToConnect, 1000);
}