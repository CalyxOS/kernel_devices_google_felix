dtb-$(CONFIG_SOC_GS201) += \
        gs201-a0.dtb \
        gs201-b0.dtb \

dtbo-y += \
        gs201-felix-dev1_0.dtbo \
        gs201-felix-proto1_0.dtbo \
        gs201-felix-proto1_1.dtbo

gs201-felix-dev1_0.dtbo-base := google-base/gs201-a0.dtb
gs201-felix-proto1_0.dtbo-base := google-base/gs201-b0.dtb
gs201-felix-proto1_1.dtbo-base := google-base/gs201-b0.dtb

MKDTIMG_FLAGS += --page_size=4096  --id=/:board_id --rev=/:board_rev
