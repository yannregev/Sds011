#include "sds011.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <termios.h>
#include "includes.h"


static int fd;
static uint8_t buffer[10];
static uint8_t bufferIndex = 0;

static uint8_t Sds011_Checksum(const uint8_t* data, size_t length);
static void Sds011_ClearFdBuffer(uint16_t size, const void *data);
static void Sds011_SleepCommand(uint16_t size, const void *data);

static inline uint8_t DataOk(const uint8_t* data, size_t length)
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

static uint8_t DataIsAck(const uint8_t* data, size_t length)
{
    return data[0] == 0xAA && data[1] == 0xC5;
}

static uint8_t Sds011_DataIsMeasurment(const uint8_t* data, size_t length)
{
    return data[0] == 0xAA && data[1] == 0xC0;
}

static uint8_t Sds011_DataReadyRead(void)
{
    struct pollfd input;
    input.fd = fd;
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
    unsigned char query_mode_cmd[] = {
        0xAA, 0xB4, 0x02, 0x01, 0x01, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0xFF, 0xFF, 0x02, 0xAB
    };

    write(fd, query_mode_cmd, sizeof(query_mode_cmd));
    tcdrain(fd);
}

static void QueryDataCommand(uint16_t size, const void *data)
{
    unsigned char query_data_cmd[] = {
        0xAA, 0xB4, 0x04, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0xFF, 0xFF, 0x02, 0xAB
    };

    write(fd, query_data_cmd, sizeof(query_data_cmd));
    tcdrain(fd);

    
}

static void Sds011_ReadData(uint16_t size, const void *data)
{
    if (!Sds011_DataReadyRead())
    {
        return;
    }
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - bufferIndex);
    if (bufferIndex + bytes_read < 10)
    {
        bufferIndex += bytes_read;
        return; // Not enough data yet
    }
    bufferIndex = 0; // Reset for next read
    if (!DataOk(buffer, bytes_read))
    {
        Sds011_ClearFdBuffer(0, NULL);
        return;
    }

    if (Sds011_DataIsMeasurment(buffer, bytes_read))
    {
        uint16_t pm25 = buffer[2] + (buffer[3] << 8);
        uint16_t pm10 = buffer[4] + (buffer[5] << 8);

        EventActivate(EV_AIR_QUALITY_MEASURED, sizeof(EV_AIR_QUALITY_MEASURED_t), &(EV_AIR_QUALITY_MEASURED_t){ 
            .pm10 = pm10,
            .pm25 = pm25,
        });
        return;
    }
    
    if (DataIsAck(buffer, bytes_read)) {
        return;
    }
    Sds011_ClearFdBuffer(0, NULL);

}

static void Sds011_ClearFdBuffer(uint16_t size, const void *data)
{
    uint8_t c;
    while (Sds011_DataReadyRead())
    {
        read(fd, &c, 1);
    }
}

static void Sds011_SleepCommand(uint16_t size, const void *data)
{
    unsigned char sleep_cmd[] = {
        0xAA, 0xB4, 0x06, 0x01, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0xFF, 0xFF, 0x05, 0xAB
    };

    write(fd, sleep_cmd, sizeof(sleep_cmd));
    tcdrain(fd);

}

static void Sds011_WakeCommand(void)
{
    unsigned char wake_cmd[] = {
        0xAA, 0xB4, 0x06, 0x01, 0x01, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 
        0xFF, 0xFF, 0x06, 0xAB
    };
    write(fd, wake_cmd, sizeof(wake_cmd));
    tcdrain(fd);

}

static inline uint8_t Sds011_Checksum(const uint8_t* data, size_t length)
{
    int16_t sum = 0;

    for (size_t i = 2; i < length - 2; i++)
    {
        sum += data[i];
    }
    return (sum & 0xFF);
}

static void Sds011_StartMeasurmentTask(uint16_t size, const void *data)
{
    Sds011_WakeCommand();
    AddDelayedFunction(QueryDataCommand, 30000); 
    AddDelayedFunction(Sds011_SleepCommand, 31000);
    AddDelayedFunction(Sds011_StartMeasurmentTask, 60000); // Repeat every 30 seconds

}

static void SetupSds011Parameters(uint16_t size, const void *data)
{
    Sds011_ClearFdBuffer(0, NULL);
    SetQueryMode();
    QueueFunctionCallback(Sds011_StartMeasurmentTask);
}

static void SetupUartParameters(void)
{
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0)
    {
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
    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        perror("tcsetattr");
        exit(1);
    }
}

void Sds011_Init(char* portname)
{
    fd = open(portname, O_RDWR);
    if (fd == -1)
    {
        perror("Error opening serial port");
        exit(1);
    }

    SetupUartParameters();
    Sds011_WakeCommand();
    AddPeriodicFunction(Sds011_ReadData, 100);
    AddDelayedFunction(SetupSds011Parameters, 2000); // Wait 2 seconds before setup

}