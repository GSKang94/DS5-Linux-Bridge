# 8BitDo Pro 2 D-input Integration

## Device: VID 0x2DC8, PID 0x6006, BT Classic, hid-generic

## HID Descriptor (150 bytes raw hex):
05010905a1018503050115002507463b0195017504651409398142750195048101150026ff0009300931093209359504750881020502150026ff0009c409c5950275088102050919012910150025017501951081020600ff850409237508951f81030600ff85060923953fb1020506092015002564750895018102050f0970850515002564750895049102c009020735083506090400

## Report ID 3 - Input (9 bytes):
- Hat (4-bit) + pad (4-bit)
- X, Y, Z, Rz (4x 8-bit axes)
- LT, RT (2x 8-bit triggers)
- 16 buttons (2 bytes)
- Battery (1 byte, 0-100)
Total: 10 bytes payload

## Report ID 4 - Input (31 bytes): vendor/gyro
## Report ID 5 - Output (4 bytes): rumble
## Report ID 6 - Feature (63 bytes): vendor config
