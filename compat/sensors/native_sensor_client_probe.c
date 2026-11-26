/* SPDX-License-Identifier: Apache-2.0 */
#include <sensors/oh_sensor.h>
#include <sensors/oh_sensor_type.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ProbeSlot {
    int type;
    const char *name;
    int64_t interval_ns;
    int count;
    int accuracy;
    int64_t last_ts;
    float last_values[4];
    uint32_t last_len;
};

static struct ProbeSlot g_slots[] = {
    {SENSOR_TYPE_ACCELEROMETER, "accel", 20000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_GYROSCOPE, "gyro", 20000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_AMBIENT_LIGHT, "ambient_light", 100000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_MAGNETIC_FIELD, "mag", 50000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_PROXIMITY, "proximity", 100000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_ORIENTATION, "orientation", 50000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_GRAVITY, "gravity", 50000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_ROTATION_VECTOR, "rotation_vector", 50000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_GAME_ROTATION_VECTOR, "game_rv", 50000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_PEDOMETER_DETECTION, "pedometer_detect", 100000000LL, 0, 0, 0, {0}, 0},
    {SENSOR_TYPE_PEDOMETER, "pedometer", 100000000LL, 0, 0, 0, {0}, 0},
};

static void OnSensorData(Sensor_Event *event)
{
    if (event == NULL) {
        return;
    }
    Sensor_Type type = SENSOR_TYPE_ACCELEROMETER;
    if (OH_SensorEvent_GetType(event, &type) != SENSOR_SUCCESS) {
        return;
    }
    float *data = NULL;
    uint32_t len = 0;
    if (OH_SensorEvent_GetData(event, &data, &len) != SENSOR_SUCCESS || data == NULL) {
        return;
    }
    int64_t ts = 0;
    (void)OH_SensorEvent_GetTimestamp(event, &ts);
    Sensor_Accuracy acc = SENSOR_ACCURACY_UNRELIABLE;
    (void)OH_SensorEvent_GetAccuracy(event, &acc);
    for (size_t i = 0; i < sizeof(g_slots) / sizeof(g_slots[0]); ++i) {
        if (g_slots[i].type == (int)type) {
            g_slots[i].count++;
            g_slots[i].accuracy = (int)acc;
            g_slots[i].last_ts = ts;
            g_slots[i].last_len = len > 4 ? 4 : len;
            for (uint32_t j = 0; j < g_slots[i].last_len; ++j) {
                g_slots[i].last_values[j] = data[j];
            }
            break;
        }
    }
}

int main(int argc, char **argv)
{
    int wait_sec = argc > 1 ? atoi(argv[1]) : 2;
    if (wait_sec <= 0 || wait_sec > 15) {
        wait_sec = 2;
    }

    uint32_t count = 0;
    Sensor_Info **infos = NULL;
    int32_t rc = OH_Sensor_GetInfos(NULL, &count);
    if (rc != SENSOR_SUCCESS || count == 0) {
        printf("SENSOR_CLIENT_GET_INFOS_COUNT rc=%d count=%u\n", rc, count);
        return 1;
    }
    infos = OH_Sensor_CreateInfos(count);
    if (infos == NULL) {
        puts("SENSOR_CLIENT_CREATE_INFOS_FAIL");
        return 2;
    }
    rc = OH_Sensor_GetInfos(infos, &count);
    printf("SENSOR_CLIENT_INFOS rc=%d count=%u\n", rc, count);
    if (rc != SENSOR_SUCCESS) {
        OH_Sensor_DestroyInfos(infos, count);
        return 3;
    }

    int present[300] = {0};
    for (uint32_t i = 0; i < count; ++i) {
        char name[128] = {0};
        char vendor[128] = {0};
        uint32_t nlen = sizeof(name);
        uint32_t vlen = sizeof(vendor);
        Sensor_Type stype = (Sensor_Type)0;
        float res = 0.0f;
        int64_t min_ns = 0, max_ns = 0;
        OH_SensorInfo_GetName(infos[i], name, &nlen);
        OH_SensorInfo_GetVendorName(infos[i], vendor, &vlen);
        OH_SensorInfo_GetType(infos[i], &stype);
        OH_SensorInfo_GetResolution(infos[i], &res);
        OH_SensorInfo_GetMinSamplingInterval(infos[i], &min_ns);
        OH_SensorInfo_GetMaxSamplingInterval(infos[i], &max_ns);
        if ((int)stype > 0 && (int)stype < 300) {
            present[(int)stype] = 1;
        }
        printf("INFO idx=%u type=%d name='%s' vendor='%s' res=%.6g min_ns=%" PRId64 " max_ns=%" PRId64 "\n",
               i, (int)stype, name, vendor, res, min_ns, max_ns);
    }
    OH_Sensor_DestroyInfos(infos, count);

    Sensor_Subscriber *sub = OH_Sensor_CreateSubscriber();
    if (sub == NULL || OH_SensorSubscriber_SetCallback(sub, OnSensorData) != SENSOR_SUCCESS) {
        puts("SENSOR_CLIENT_SUBSCRIBER_FAIL");
        return 4;
    }

    int subscribed[sizeof(g_slots) / sizeof(g_slots[0])] = {0};
    for (size_t i = 0; i < sizeof(g_slots) / sizeof(g_slots[0]); ++i) {
        int t = g_slots[i].type;
        if (t <= 0 || t >= 300 || !present[t]) {
            continue;
        }
        Sensor_SubscriptionId *sid = OH_Sensor_CreateSubscriptionId();
        Sensor_SubscriptionAttribute *attr = OH_Sensor_CreateSubscriptionAttribute();
        OH_SensorSubscriptionId_SetType(sid, (Sensor_Type)t);
        OH_SensorSubscriptionAttribute_SetSamplingInterval(attr, g_slots[i].interval_ns);
        int32_t src = OH_Sensor_Subscribe(sid, attr, sub);
        printf("SUBSCRIBE type=%d (%s) rc=%d\n", t, g_slots[i].name, src);
        subscribed[i] = (src == SENSOR_SUCCESS);
        OH_Sensor_DestroySubscriptionAttribute(attr);
        OH_Sensor_DestroySubscriptionId(sid);
    }

    sleep((unsigned)wait_sec);

    for (size_t i = 0; i < sizeof(g_slots) / sizeof(g_slots[0]); ++i) {
        if (!subscribed[i]) {
            continue;
        }
        Sensor_SubscriptionId *sid = OH_Sensor_CreateSubscriptionId();
        OH_SensorSubscriptionId_SetType(sid, (Sensor_Type)g_slots[i].type);
        (void)OH_Sensor_Unsubscribe(sid, sub);
        OH_Sensor_DestroySubscriptionId(sid);
        printf("SAMPLE type=%d (%s) count=%d acc=%d ts=%" PRId64 " len=%u vals=[%.4f,%.4f,%.4f,%.4f]\n",
               g_slots[i].type, g_slots[i].name, g_slots[i].count, g_slots[i].accuracy,
               g_slots[i].last_ts, g_slots[i].last_len,
               g_slots[i].last_values[0], g_slots[i].last_values[1],
               g_slots[i].last_values[2], g_slots[i].last_values[3]);
    }

    OH_Sensor_DestroySubscriber(sub);
    return 0;
}
