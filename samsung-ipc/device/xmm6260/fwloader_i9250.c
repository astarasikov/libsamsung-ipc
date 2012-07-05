/*
 * Firmware loader for Samsung I9100 and I9250
 * Copyright (C) 2012 Alexander Tarasikov <alexander.tarasikov@gmail.com>
 *
 * based on the incomplete C++ implementation which is
 * Copyright (C) 2012 Sergey Gridasov <grindars@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "modemctl_common.h"
#include "ipc_private.h"
#include "fwloader_i9250.h"

/*
 * Locations of the firmware components in the Samsung firmware
 */

struct xmm6260_radio_part i9250_radio_parts[] = {
    [PSI] = {
        .offset = 0,
        .length = 0xf000,
    },
    [EBL] = {
        .offset = 0xf000,
        .length = 0x19000,
    },
    [SECURE_IMAGE] = {
        .offset = 0x9ff800,
        .length = 0x800,
    },
    [FIRMWARE] = {
        .offset = 0x28000,
        .length = 0x9d8000,
    },
    [NVDATA] = {
        .offset = 0xa00000,
        .length = 2 << 20,
    }
};

struct i9250_boot_cmd_desc i9250_boot_cmd_desc[] = {
    [SetPortConf] = {
        .code = 0x86,
        .long_tail = 1,
    },
    [ReqSecStart] = {
        .code = 0x204,
        .long_tail = 1,
    },
    [ReqSecEnd] = {
        .code = 0x205,
    },
    [ReqForceHwReset] = {
        .code = 0x208,
        .long_tail = 1,
        .no_ack = 1,
    },
    [ReqFlashSetAddress] = {
        .code = 0x802,
        .long_tail = 1,
    },
    [ReqFlashWriteBlock] = {
        .code = 0x804,
    },
};

static int reboot_modem_i9250(struct ipc_client *client,
    struct modemctl_io_data *io_data, bool hard) {
    int ret;

    if (!hard) {
        return 0;
    }

    /*
     * Disable the hardware to ensure consistent state
     */
    if ((ret = modemctl_modem_power(client, io_data, false)) < 0) {
        _e("failed to disable modem power");
        goto fail;
    }
    else {
        _d("disabled modem power");
    }

    if ((ret = modemctl_modem_boot_power(client, io_data, false)) < 0) {
        _e("failed to disable modem boot power");
        goto fail;
    }
    else {
        _d("disabled modem boot power");
    }

    /*
     * Now, initialize the hardware
     */
    if ((ret = modemctl_modem_boot_power(client, io_data, true)) < 0) {
        _e("failed to enable modem boot power");
        goto fail;
    }
    else {
        _d("enabled modem boot power");
    }

    if ((ret = modemctl_modem_power(client, io_data, true)) < 0) {
        _e("failed to enable modem power");
        goto fail;
    }
    else {
        _d("enabled modem power");
    }

fail:
    return ret;
}

static int send_image_i9250(struct ipc_client *client,
    struct modemctl_io_data *io_data, enum xmm6260_image type) {
    int ret = -1;

    if (type >= ARRAY_SIZE(i9250_radio_parts)) {
        _e("bad image type %x", type);
        goto fail;
    }

    size_t length = i9250_radio_parts[type].length;
    size_t offset = i9250_radio_parts[type].offset;

    size_t start = offset;
    size_t end = length + start;

    unsigned char crc = calculateCRC(io_data->radio_data, offset, length);

    //dump some image bytes
    _d("image start");
    hexdump(io_data->radio_data + start, length);

    size_t chunk_size = 0xdfc;

    while (start < end) {
        size_t remaining = end - start;
        size_t curr_chunk = chunk_size < remaining ? chunk_size : remaining;
        ret = write(io_data->boot_fd, io_data->radio_data + start, curr_chunk);
        if (ret < 0) {
            _e("failed to write image chunk");
            goto fail;
        }
        start += ret;
    }
    _d("sent image type=%d", type);

    if (type == EBL) {
        if ((ret = write(io_data->boot_fd, &crc, 1)) < 1) {
            _e("failed to write EBL CRC");
            goto fail;
        }
        else {
            _d("wrote EBL CRC %02x", crc);
        }
        goto done;
    }

    uint32_t crc32 = (crc << 24) | 0xffffff;
    if ((ret = write(io_data->boot_fd, &crc32, 4)) != 4) {
        _e("failed to write CRC");
        goto fail;
    }
    else {
        _d("wrote CRC %x", crc);
    }

done:
    ret = 0;

fail:
    return ret;
}

static int send_PSI_i9250(struct ipc_client *client,
    struct modemctl_io_data *io_data) {
    int ret = -1;

    if ((ret = write(io_data->boot_fd, I9250_PSI_START_MAGIC, 4)) < 0) {
        _d("%s: failed to write header, ret %d", __func__, ret);
        goto fail;
    }

    if ((ret = send_image_i9250(client, io_data, PSI)) < 0) {
        _e("failed to send PSI image");
        goto fail;
    }

    char expected_acks[4][4] = {
        "\xff\xff\xff\x01",
        "\xff\xff\xff\x01",
        "\x02\x00\x00\x00",
        "\x01\xdd\x00\x00",
    };

    unsigned i;
    for (i = 0; i < ARRAY_SIZE(expected_acks); i++) {
        ret = expect_data(io_data->boot_fd, expected_acks[i], 4);
        if (ret < 0) {
            _d("failed to wait for ack %d", i);
            goto fail;
        }
    }
    _d("received PSI ACK");

    return 0;

fail:
    return ret;
}

static int send_EBL_i9250(struct ipc_client *client,
    struct modemctl_io_data *io_data) {
    int ret;
    int fd = io_data->boot_fd;
    unsigned length = i9250_radio_parts[EBL].length;

    if ((ret = write(fd, "\x04\x00\x00\x00", 4)) != 4) {
        _e("failed to write length of EBL length ('4') ");
        goto fail;
    }

    if ((ret = write(fd, &length, sizeof(length))) != sizeof(length)) {
        _e("failed to write EBL length");
        goto fail;
    }

    if ((ret = expect_data(fd, I9250_GENERAL_ACK, 4)) < 0) {
        _e("failed to wait for EBL length ACK");
        goto fail;
    }

    if ((ret = expect_data(fd, I9250_EBL_HDR_ACK_MAGIC, 4)) < 0) {
        _e("failed to wait for EBL header ACK");
        goto fail;
    }

    length++;
    if ((ret = write(fd, &length, sizeof(length))) != sizeof(length)) {
        _e("failed to write EBL length + 1");
        goto fail;
    }

    if ((ret = send_image_i9250(client, io_data, EBL)) < 0) {
        _e("failed to send EBL image");
        goto fail;
    }
    else {
        _d("sent EBL image, waiting for ACK");
    }

    if ((ret = expect_data(fd, I9250_GENERAL_ACK, 4)) < 0) {
        _e("failed to wait for EBL image general ACK");
        goto fail;
    }

    if ((ret = expect_data(fd, I9250_EBL_IMG_ACK_MAGIC, 4)) < 0) {
        _e("failed to wait for EBL image ACK");
        goto fail;
    }
    else {
        _d("got EBL ACK");
    }

    return 0;

fail:
    return ret;
}

static int bootloader_cmd(struct ipc_client *client,
    struct modemctl_io_data *io_data, enum xmm6260_boot_cmd cmd,
    void *data, size_t data_size)
{
    int ret = 0;
    char *cmd_data = 0;
    if (cmd >= ARRAY_SIZE(i9250_boot_cmd_desc)) {
        _e("bad command %x\n", cmd);
        goto done_or_fail;
    }

    unsigned cmd_code = i9250_boot_cmd_desc[cmd].code;

    uint16_t checksum = (data_size & 0xffff) + cmd_code;
    unsigned char *ptr = (unsigned char*)data;
    size_t i;
    for (i = 0; i < data_size; i++) {
        checksum += ptr[i];
    }

    DECLARE_BOOT_CMD_HEADER(header, cmd_code, data_size);
    DECLARE_BOOT_TAIL_HEADER(tail, checksum);

    size_t tail_size = sizeof(tail);
    if (!i9250_boot_cmd_desc[cmd].long_tail) {
        tail_size -= 2;
    }

    size_t cmd_buffer_size = data_size + sizeof(header) + tail_size;
    _d("data_size %d [%d] checksum 0x%x", data_size, cmd_buffer_size, checksum);

    cmd_data = (char*)malloc(cmd_buffer_size);
    if (!cmd_data) {
        _e("failed to allocate command buffer");
        ret = -ENOMEM;
        goto done_or_fail;
    }
    memset(cmd_data, 0, cmd_buffer_size);
    memcpy(cmd_data, &header, sizeof(header));
    memcpy(cmd_data + sizeof(header), data, data_size);
    memcpy(cmd_data + sizeof(header) + data_size, &tail, tail_size);

    _d("bootloader cmd packet");
    hexdump(cmd_data, cmd_buffer_size);
    hexdump(cmd_data + cmd_buffer_size - 16, 16);

    if ((ret = write(io_data->boot_fd, cmd_data, cmd_buffer_size)) < 0) {
        _e("failed to write command to socket");
        goto done_or_fail;
    }

    if ((unsigned)ret < cmd_buffer_size) {
        _e("written %d bytes of %d", ret, cmd_buffer_size);
        ret = -EINVAL;
        goto done_or_fail;
    }

    _d("sent command %x", header.cmd);
    if (i9250_boot_cmd_desc[cmd].no_ack) {
        _i("not waiting for ACK");
        goto done_or_fail;
    }

    uint32_t ack_length;
    if ((ret = receive(io_data->boot_fd, &ack_length, 4)) < 0) {
        _e("failed to receive ack header length");
        goto done_or_fail;
    }

    if (ack_length + 4 > cmd_buffer_size) {
        free(cmd_data);
        cmd_data = NULL;
        cmd_data = malloc(ack_length + 4);
        if (!cmd_data) {
            _e("failed to allocate the buffer for ack data");
            goto done_or_fail;
        }
    }
    memset(cmd_data, 0, ack_length);
    memcpy(cmd_data, &ack_length, 4);
    for (i = 0; i < (ack_length + 3) / 4; i++) {
        if ((ret = receive(io_data->boot_fd, cmd_data + ((i + 1) << 2), 4)) < 0) {
            _e("failed to receive ack chunk");
            goto done_or_fail;
        }
    }

    _d("received ack");
    hexdump(cmd_data, ack_length + 4);

    struct i9250_boot_cmd_header *ack_hdr = (struct i9250_boot_cmd_header*)cmd_data;
    struct i9250_boot_tail_header *ack_tail = (struct i9250_boot_tail_header*)
        (cmd_data + ack_length + 4 - sizeof(struct i9250_boot_tail_header));

    _d("ack code 0x%x checksum 0x%x", ack_hdr->cmd, ack_tail->checksum);
    if (ack_hdr->cmd != header.cmd) {
        _e("request and ack command codes do not match");
        ret = -1;
        goto done_or_fail;
    }

    ret = 0;

done_or_fail:

    if (cmd_data) {
        free(cmd_data);
    }

    return ret;
}

static int ack_BootInfo_i9250(struct ipc_client *client,
    struct modemctl_io_data *io_data) {
    int ret = -1;
    uint32_t boot_info_length;
    char *boot_info = 0;


    if ((ret = receive(io_data->boot_fd, &boot_info_length, 4)) < 0) {
        _e("failed to receive boot info length");
        goto fail;
    }

    _d("Boot Info length=0x%x", boot_info_length);

    boot_info = (char*)malloc(boot_info_length);
    if (!boot_info) {
        _e("failed to allocate memory for boot info");
        goto fail;
    }

    memset(boot_info, 0, boot_info_length);

    size_t boot_chunk = 4;
    size_t boot_chunk_count = (boot_info_length + boot_chunk - 1) / boot_chunk;
    unsigned i;
    for (i = 0; i < boot_chunk_count; i++) {
        ret = receive(io_data->boot_fd, boot_info + (i * boot_chunk), boot_chunk);
        if (ret < 0) {
            _e("failed to receive Boot Info chunk %i ret=%d", i, ret);
            goto fail;
        }
    }

    _d("received Boot Info");
    hexdump(boot_info, boot_info_length);

    ret = bootloader_cmd(client, io_data, SetPortConf, boot_info, boot_info_length);
    if (ret < 0) {
        _e("failed to send SetPortConf command");
        goto fail;
    }
    else {
        _d("sent SetPortConf command");
    }

    ret = 0;

fail:
    if (boot_info) {
        free(boot_info);
    }

    return ret;
}

static int send_secure_data(struct ipc_client *client,
    struct modemctl_io_data *io_data, uint32_t addr,
    void *data, int data_len)
{
    int ret = 0;
    int count = 0;
    char *data_p = (char *) data;

    if ((ret = bootloader_cmd(client, io_data, ReqFlashSetAddress, &addr, 4)) < 0) {
        _e("failed to send ReqFlashSetAddress");
        goto fail;
    }
    else {
        _d("sent ReqFlashSetAddress");
    }

    while (count < data_len) {
        int rest = data_len - count;
        int chunk = rest < SEC_DOWNLOAD_CHUNK ? rest : SEC_DOWNLOAD_CHUNK;

        ret = bootloader_cmd(client, io_data, ReqFlashWriteBlock, data_p, chunk);
        if (ret < 0) {
            _e("failed to send data chunk");
            goto fail;
        }

        data_p += chunk;
        count += chunk;
    }

    usleep(SEC_DOWNLOAD_DELAY_US);

fail:
    return ret;
}

static int send_secure_image(struct ipc_client *client,
    struct modemctl_io_data *io_data, uint32_t addr, enum xmm6260_image type)
{
    uint32_t offset = i9250_radio_parts[type].offset;
    uint32_t length = i9250_radio_parts[type].length;
    char *start = io_data->radio_data + offset;
    int ret = 0;

    ret = send_secure_data(client, io_data, addr, start, length);

    return ret;
}

static int send_mps_data(struct ipc_client *client,
    struct modemctl_io_data *io_data) {
    int ret = 0;
    int mps_fd = -1;
    char mps_data[I9250_MPS_LENGTH] = {};
    uint32_t addr = I9250_MPS_LOAD_ADDR;

    mps_fd = open(I9250_MPS_IMAGE_PATH, O_RDONLY);
    if (mps_fd < 0) {
        _e("failed to open MPS data");
    }
    else {
        read(mps_fd, mps_data, I9250_MPS_LENGTH);
    }

    if ((ret = bootloader_cmd(client, io_data, ReqFlashSetAddress, &addr, 4)) < 0) {
        _e("failed to send ReqFlashSetAddress");
        goto fail;
    }
    else {
        _d("sent ReqFlashSetAddress");
    }

    if ((ret = bootloader_cmd(client, io_data, ReqFlashWriteBlock,
        mps_data, I9250_MPS_LENGTH)) < 0) {
        _e("failed to write MPS data to modem");
        goto fail;
    }


fail:
    if (mps_fd >= 0) {
        close(mps_fd);
    }

    return ret;
}

static int send_SecureImage_i9250(struct ipc_client *client,
    struct modemctl_io_data *io_data) {
    int ret = 0;

    uint32_t sec_off = i9250_radio_parts[SECURE_IMAGE].offset;
    uint32_t sec_len = i9250_radio_parts[SECURE_IMAGE].length;
    void *sec_img = io_data->radio_data + sec_off;
    void *nv_data = NULL;

    if ((ret = bootloader_cmd(client, io_data, ReqSecStart, sec_img, sec_len)) < 0) {
        _e("failed to write ReqSecStart");
        goto fail;
    }
    else {
        _d("sent ReqSecStart");
    }

    if ((ret = send_secure_image(client, io_data, FW_LOAD_ADDR, FIRMWARE)) < 0) {
        _e("failed to send FIRMWARE image");
        goto fail;
    }
    else {
        _d("sent FIRMWARE image");
    }

    nv_data_check(client);
    nv_data_md5_check(client);

    nv_data = ipc_file_read(client, nv_data_path(client), 2 << 20, 1024);
    if (nv_data == NULL) {
        _e("failed to read NVDATA image");
        goto fail;
    }

    if ((ret = send_secure_data(client, io_data, NVDATA_LOAD_ADDR, nv_data, 2 << 20)) < 0) {
        _e("failed to send NVDATA image");
        goto fail;
    }
    else {
        _d("sent NVDATA image");
    }

    free(nv_data);

    if ((ret = send_mps_data(client, io_data)) < 0) {
        _e("failed to send MPS data");
        goto fail;
    }
    else {
        _d("sent MPS data");
    }

    if ((ret = bootloader_cmd(client, io_data, ReqSecEnd,
        BL_END_MAGIC, BL_END_MAGIC_LEN)) < 0)
    {
        _e("failed to write ReqSecEnd");
        goto fail;
    }
    else {
        _d("sent ReqSecEnd");
    }

    ret = bootloader_cmd(client, io_data, ReqForceHwReset,
        BL_RESET_MAGIC, BL_RESET_MAGIC_LEN);
    if (ret < 0) {
        _e("failed to write ReqForceHwReset");
        goto fail;
    }
    else {
        _d("sent ReqForceHwReset");
    }

fail:
    return ret;
}

int boot_modem_i9250(struct ipc_client *client) {
    int ret = -1;
    struct modemctl_io_data io_data;
    memset(&io_data, 0, sizeof(client, io_data));

    io_data.radio_fd = open(I9250_RADIO_IMAGE, O_RDONLY);
    if (io_data.radio_fd < 0) {
        _e("failed to open radio firmware");
        goto fail;
    }
    else {
        _d("opened radio image %s, fd=%d", I9250_RADIO_IMAGE, io_data.radio_fd);
    }

    if (fstat(io_data.radio_fd, &io_data.radio_stat) < 0) {
        _e("failed to stat radio image, error %s", strerror(errno));
        goto fail;
    }

    io_data.radio_data = mmap(0, RADIO_MAP_SIZE, PROT_READ, MAP_SHARED,
        io_data.radio_fd, 0);
    if (io_data.radio_data == MAP_FAILED) {
        _e("failed to mmap radio image, error %s", strerror(errno));
        goto fail;
    }

    io_data.boot_fd = open(BOOT_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (io_data.boot_fd < 0) {
        _e("failed to open boot device");
        goto fail;
    }
    else {
        _d("opened boot device %s, fd=%d", BOOT_DEV, io_data.boot_fd);
    }

    if (reboot_modem_i9250(client, &io_data, true) < 0) {
        _e("failed to hard reset modem");
        goto fail;
    }
    else {
        _d("modem hard reset done");
    }

    /*
     * Now, actually load the firmware
     */
    int i;
    for (i = 0; i < 2; i++) {
        if (write(io_data.boot_fd, "ATAT", 4) != 4) {
            _e("failed to write ATAT to boot socket");
            goto fail;
        }
        else {
            _d("written ATAT to boot socket, waiting for ACK");
        }

        if (read_select(io_data.boot_fd, 100) < 0) {
            _d("failed to select before next ACK, ignoring");
        }
    }

    //FIXME: make sure it does not timeout or add the retry in the ril library

    if ((ret = read_select(io_data.boot_fd, 100)) < 0) {
        _e("failed to wait for bootloader ready state");
        goto fail;
    }
    else {
        _d("ready for PSI upload");
    }

    ret = -ETIMEDOUT;
    for (i = 0; i < I9250_BOOT_REPLY_MAX; i++) {
        uint32_t id_buf;
        if ((ret = receive(io_data.boot_fd, (void*)&id_buf, 4)) != 4) {
            _e("failed receiving bootloader reply");
            goto fail;
        }
        _d("got bootloader reply %08x", id_buf);
        if (id_buf == I9250_BOOT_LAST_MARKER) {
            ret = 0;
            break;
        }
    }

    if (ret < 0) {
        _e("bootloader id marker not received");
        goto fail;
    }
    else {
        _d("got bootloader id marker");
    }

    if ((ret = send_PSI_i9250(client, &io_data)) < 0) {
        _e("failed to upload PSI");
        goto fail;
    }
    else {
        _d("PSI download complete");
    }

    close(io_data.boot_fd);
    io_data.boot_fd = open(I9250_SECOND_BOOT_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (io_data.boot_fd < 0) {
        _e("failed to open " I9250_SECOND_BOOT_DEV " control device");
        goto fail;
    }
    else {
        _d("opened second boot device %s, fd=%d", I9250_SECOND_BOOT_DEV, io_data.boot_fd);
    }

    //RpsiCmdLoadAndExecute
    if ((ret = write(io_data.boot_fd, I9250_PSI_CMD_EXEC, 4)) < 0) {
        _e("failed writing cmd_load_exe_EBL");
        goto fail;
    }
    if ((ret = write(io_data.boot_fd, I9250_PSI_EXEC_DATA, 8)) < 0) {
        _e("failed writing 8 bytes to boot1");
        goto fail;
    }

    if ((ret = expect_data(io_data.boot_fd, I9250_GENERAL_ACK, 4)) < 0) {
        _e("failed to receive cmd_load_exe_EBL ack");
        goto fail;
    }

    if ((ret = expect_data(io_data.boot_fd, I9250_PSI_READY_ACK, 4)) < 0) {
        _e("failed to receive PSI ready ack");
        goto fail;
    }

    if ((ret = send_EBL_i9250(client, &io_data)) < 0) {
        _e("failed to upload EBL");
        goto fail;
    }
    else {
        _d("EBL download complete");
    }

    if ((ret = ack_BootInfo_i9250(client, &io_data)) < 0) {
        _e("failed to receive Boot Info");
        goto fail;
    }
    else {
        _d("Boot Info ACK done");
    }

    if ((ret = send_SecureImage_i9250(client, &io_data)) < 0) {
        _e("failed to upload Secure Image");
        goto fail;
    }
    else {
        _d("Secure Image download complete");
    }

    if ((ret = modemctl_wait_modem_online(client, &io_data))) {
        _e("failed to wait for modem to become online");
        goto fail;
    }

    _i("modem online");
    ret = 0;

fail:
    if (io_data.radio_data != MAP_FAILED) {
        munmap(io_data.radio_data, RADIO_MAP_SIZE);
    }

    if (io_data.radio_fd >= 0) {
        close(io_data.radio_fd);
    }

    if (io_data.boot_fd >= 0) {
        close(io_data.boot_fd);
    }

    return ret;
}

// vim:ts=4:sw=4:expandtab
