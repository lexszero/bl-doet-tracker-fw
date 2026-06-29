#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include <app/diagnostic_log.h>

LOG_MODULE_REGISTER(diagnostic_log, CONFIG_APP_LOG_LEVEL);

#define DIAGNOSTIC_LOG_MAGIC 0xd107U
#define DIAGNOSTIC_LOG_VERSION 1U
#define DIAGNOSTIC_LOG_RECORD_SIZE 40U
#define DIAGNOSTIC_LOG_SECTOR_SIZE 4096U

struct diagnostic_log_record {
	uint16_t magic;
	uint8_t version;
	uint8_t record_size;
	uint32_t seq;
	uint32_t uptime_ms;
	uint32_t utc_packed;
	int32_t latitude;
	int32_t longitude;
	uint16_t speed_cm_s;
	uint16_t hdop;
	uint16_t interval_s;
	int16_t send_ret;
	uint8_t motion_state;
	uint8_t result;
	uint8_t satellites;
	uint8_t flags;
	uint16_t crc16;
	uint16_t reserved;
} __attribute__((packed));

BUILD_ASSERT(sizeof(struct diagnostic_log_record) == DIAGNOSTIC_LOG_RECORD_SIZE);

#if IS_ENABLED(CONFIG_TRACKER_DIAGNOSTIC_LOG)

#define DIAGNOSTIC_LOG_PARTITION_NODE DT_NODELABEL(diagnostic_log_partition)

BUILD_ASSERT(DT_NODE_EXISTS(DIAGNOSTIC_LOG_PARTITION_NODE),
	     "diagnostic_log_partition is required when CONFIG_TRACKER_DIAGNOSTIC_LOG=y");
BUILD_ASSERT((DT_REG_SIZE(DIAGNOSTIC_LOG_PARTITION_NODE) % DIAGNOSTIC_LOG_SECTOR_SIZE) == 0);

#define DIAGNOSTIC_LOG_AREA_ID PARTITION_ID(diagnostic_log_partition)
#define DIAGNOSTIC_LOG_PARTITION_SIZE DT_REG_SIZE(DIAGNOSTIC_LOG_PARTITION_NODE)
#define DIAGNOSTIC_LOG_SECTOR_COUNT \
	(DIAGNOSTIC_LOG_PARTITION_SIZE / DIAGNOSTIC_LOG_SECTOR_SIZE)
#define DIAGNOSTIC_LOG_RECORDS_PER_SECTOR \
	(DIAGNOSTIC_LOG_SECTOR_SIZE / DIAGNOSTIC_LOG_RECORD_SIZE)
#define DIAGNOSTIC_LOG_RECORD_CAPACITY \
	(DIAGNOSTIC_LOG_SECTOR_COUNT * DIAGNOSTIC_LOG_RECORDS_PER_SECTOR)

static const struct flash_area *diagnostic_log_area;
static bool diagnostic_log_ready;
static uint32_t diagnostic_log_write_index;
static uint32_t diagnostic_log_next_seq;

static uint16_t diagnostic_log_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xffffU;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int bit = 0; bit < 8; bit++) {
			if ((crc & 0x8000U) != 0U) {
				crc = (crc << 1) ^ 0x1021U;
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

static bool diagnostic_log_record_is_erased(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (data[i] != 0xffU) {
			return false;
		}
	}

	return true;
}

static bool diagnostic_log_record_is_valid(const struct diagnostic_log_record *record)
{
	uint16_t crc;

	if (record->magic != DIAGNOSTIC_LOG_MAGIC ||
	    record->version != DIAGNOSTIC_LOG_VERSION ||
	    record->record_size != DIAGNOSTIC_LOG_RECORD_SIZE) {
		return false;
	}

	crc = diagnostic_log_crc16((const uint8_t *)record,
				   offsetof(struct diagnostic_log_record, crc16));

	return record->crc16 == crc;
}

static uint32_t diagnostic_log_offset_for_index(uint32_t index)
{
	uint32_t sector = index / DIAGNOSTIC_LOG_RECORDS_PER_SECTOR;
	uint32_t slot = index % DIAGNOSTIC_LOG_RECORDS_PER_SECTOR;

	return (sector * DIAGNOSTIC_LOG_SECTOR_SIZE) +
	       (slot * DIAGNOSTIC_LOG_RECORD_SIZE);
}

static int diagnostic_log_erase_sector(uint32_t sector)
{
	return flash_area_erase(diagnostic_log_area,
				sector * DIAGNOSTIC_LOG_SECTOR_SIZE,
				DIAGNOSTIC_LOG_SECTOR_SIZE);
}

static uint32_t diagnostic_log_sector_for_index(uint32_t index)
{
	return index / DIAGNOSTIC_LOG_RECORDS_PER_SECTOR;
}

static uint32_t diagnostic_log_first_index_in_sector(uint32_t sector)
{
	return sector * DIAGNOSTIC_LOG_RECORDS_PER_SECTOR;
}

static int diagnostic_log_prepare_write_slot(void)
{
	struct diagnostic_log_record record;
	uint32_t offset;
	uint32_t sector;
	uint32_t slot;
	int ret;

	offset = diagnostic_log_offset_for_index(diagnostic_log_write_index);
	ret = flash_area_read(diagnostic_log_area, offset, &record, sizeof(record));
	if (ret != 0) {
		return ret;
	}

	if (diagnostic_log_record_is_erased((const uint8_t *)&record, sizeof(record))) {
		return 0;
	}

	sector = diagnostic_log_sector_for_index(diagnostic_log_write_index);
	slot = diagnostic_log_write_index % DIAGNOSTIC_LOG_RECORDS_PER_SECTOR;

	if (slot != 0U) {
		sector = (sector + 1U) % DIAGNOSTIC_LOG_SECTOR_COUNT;
		diagnostic_log_write_index = diagnostic_log_first_index_in_sector(sector);
	}

	return diagnostic_log_erase_sector(sector);
}

int diagnostic_log_init(void)
{
	struct diagnostic_log_record record;
	bool have_record = false;
	uint32_t latest_seq = 0U;
	uint32_t latest_index = 0U;
	int ret;

	ret = flash_area_open(DIAGNOSTIC_LOG_AREA_ID, &diagnostic_log_area);
	if (ret != 0) {
		LOG_WRN("diagnostic log flash area open failed: %d", ret);
		return ret;
	}

	for (uint32_t index = 0U; index < DIAGNOSTIC_LOG_RECORD_CAPACITY; index++) {
		uint32_t offset = diagnostic_log_offset_for_index(index);

		ret = flash_area_read(diagnostic_log_area, offset, &record, sizeof(record));
		if (ret != 0) {
			LOG_WRN("diagnostic log scan failed at 0x%x: %d", offset, ret);
			return ret;
		}

		if (!diagnostic_log_record_is_valid(&record)) {
			continue;
		}

		if (!have_record || record.seq > latest_seq) {
			have_record = true;
			latest_seq = record.seq;
			latest_index = index;
		}
	}

	if (have_record) {
		diagnostic_log_next_seq = latest_seq + 1U;
		diagnostic_log_write_index = (latest_index + 1U) %
					     DIAGNOSTIC_LOG_RECORD_CAPACITY;
	} else {
		diagnostic_log_next_seq = 0U;
		diagnostic_log_write_index = 0U;
	}

	diagnostic_log_ready = true;
	LOG_INF("diagnostic log ready: %u KiB, %u records, next seq %u",
		DIAGNOSTIC_LOG_PARTITION_SIZE / 1024U,
		DIAGNOSTIC_LOG_RECORD_CAPACITY,
		diagnostic_log_next_seq);

	return 0;
}

int diagnostic_log_write_uplink(const struct diagnostic_log_uplink_entry *entry)
{
	struct diagnostic_log_record record = {0};
	uint32_t offset;
	int ret;

	if (!diagnostic_log_ready || entry == NULL) {
		return -ENODEV;
	}

	ret = diagnostic_log_prepare_write_slot();
	if (ret != 0) {
		LOG_WRN("diagnostic log write slot prepare failed: %d", ret);
		return ret;
	}

	record.magic = DIAGNOSTIC_LOG_MAGIC;
	record.version = DIAGNOSTIC_LOG_VERSION;
	record.record_size = DIAGNOSTIC_LOG_RECORD_SIZE;
	record.seq = diagnostic_log_next_seq;
	record.uptime_ms = entry->uptime_ms;
	record.utc_packed = entry->utc_packed;
	record.latitude = entry->latitude;
	record.longitude = entry->longitude;
	record.speed_cm_s = entry->speed_cm_s;
	record.hdop = entry->hdop;
	record.interval_s = entry->interval_s;
	record.send_ret = entry->send_ret;
	record.motion_state = entry->motion_state;
	record.result = entry->result;
	record.satellites = entry->satellites;
	record.flags = entry->flags;
	record.crc16 = diagnostic_log_crc16((const uint8_t *)&record,
					    offsetof(struct diagnostic_log_record, crc16));

	offset = diagnostic_log_offset_for_index(diagnostic_log_write_index);
	ret = flash_area_write(diagnostic_log_area, offset, &record, sizeof(record));
	if (ret != 0) {
		LOG_WRN("diagnostic log write failed at 0x%x: %d", offset, ret);
		return ret;
	}

	diagnostic_log_next_seq++;
	diagnostic_log_write_index = (diagnostic_log_write_index + 1U) %
				     DIAGNOSTIC_LOG_RECORD_CAPACITY;

	return 0;
}

#else

int diagnostic_log_init(void)
{
	return 0;
}

int diagnostic_log_write_uplink(const struct diagnostic_log_uplink_entry *entry)
{
	ARG_UNUSED(entry);

	return 0;
}

#endif

uint32_t diagnostic_log_pack_utc(const struct gnss_time *utc)
{
	uint32_t second;

	if (utc == NULL) {
		return 0U;
	}

	second = utc->millisecond / 1000U;

	if (utc->century_year > 63U || utc->month < 1U || utc->month > 12U ||
	    utc->month_day < 1U || utc->month_day > 31U ||
	    utc->hour > 23U || utc->minute > 59U || second > 59U) {
		return 0U;
	}

	return ((uint32_t)utc->century_year << 26) |
	       ((uint32_t)utc->month << 22) |
	       ((uint32_t)utc->month_day << 17) |
	       ((uint32_t)utc->hour << 12) |
	       ((uint32_t)utc->minute << 6) |
	       second;
}
