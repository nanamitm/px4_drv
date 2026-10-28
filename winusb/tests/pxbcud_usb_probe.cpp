// SPDX-License-Identifier: GPL-2.0-only
// Hub 経由の読み取りだけで USB 記述子を調べる。機器のドライバー入替は不要。
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbioctl.h>
#include <usbiodef.h>
#include <cstdio>
#include <vector>
#include <cstddef>

#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"uuid.lib")

int main()
{
    HDEVINFO list=SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_HUB,nullptr,nullptr,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);
    if(list==INVALID_HANDLE_VALUE){std::printf("hub enumeration failed win32=%lu\n",GetLastError());return 1;}
    bool found=false;
    for(DWORD n=0;;++n) {
        SP_DEVICE_INTERFACE_DATA entry{};entry.cbSize=sizeof(entry);
        if(!SetupDiEnumDeviceInterfaces(list,nullptr,&GUID_DEVINTERFACE_USB_HUB,n,&entry))break;
        DWORD bytes=0;
        SetupDiGetDeviceInterfaceDetailW(list,&entry,nullptr,0,&bytes,nullptr);
        if(!bytes)continue;
        std::vector<unsigned char> path(bytes);
        auto *detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(path.data());detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailW(list,&entry,detail,bytes,nullptr,nullptr))continue;
        HANDLE hub=CreateFileW(detail->DevicePath,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(hub==INVALID_HANDLE_VALUE)continue;
        USB_NODE_INFORMATION node{};node.NodeType=UsbHub;
        if(DeviceIoControl(hub,IOCTL_USB_GET_NODE_INFORMATION,&node,sizeof(node),&node,sizeof(node),&bytes,nullptr)) {
            for(ULONG port=1;port<=node.u.HubInformation.HubDescriptor.bNumberOfPorts;++port) {
                std::vector<unsigned char> connection_buffer(sizeof(USB_NODE_CONNECTION_INFORMATION_EX)+30*sizeof(USB_PIPE_INFO));
                auto &connection=*reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(connection_buffer.data());
                connection.ConnectionIndex=port;
                if(!DeviceIoControl(hub,IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,&connection,static_cast<DWORD>(connection_buffer.size()),&connection,static_cast<DWORD>(connection_buffer.size()),&bytes,nullptr))continue;
                const auto &d=connection.DeviceDescriptor;
                if(d.idVendor!=0x3275 || d.idProduct!=0x0085)continue;
                found=true;
                std::printf("PX-BCUD port=%lu VID=%04x PID=%04x revision=%04x speed=%u configurations=%u\n",port,d.idVendor,d.idProduct,d.bcdDevice,connection.Speed,d.bNumConfigurations);
                std::vector<unsigned char> buffer(sizeof(USB_DESCRIPTOR_REQUEST)+4096);
                auto *request=reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(buffer.data());
                request->ConnectionIndex=port;
                request->SetupPacket.bmRequest=0x80;request->SetupPacket.bRequest=USB_REQUEST_GET_DESCRIPTOR;
                request->SetupPacket.wValue=USB_CONFIGURATION_DESCRIPTOR_TYPE<<8;request->SetupPacket.wLength=4096;
                if(!DeviceIoControl(hub,IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,request,static_cast<DWORD>(buffer.size()),request,static_cast<DWORD>(buffer.size()),&bytes,nullptr)) {
                    std::printf("configuration descriptor failed win32=%lu\n",GetLastError());continue;
                }
                const std::size_t header=offsetof(USB_DESCRIPTOR_REQUEST,Data);
                for(std::size_t offset=header;offset+2<=bytes;) {
                    const auto *p=buffer.data()+offset;
                    if(p[0]<2 || offset+p[0]>bytes)break;
                    if(p[1]==USB_INTERFACE_DESCRIPTOR_TYPE && p[0]>=sizeof(USB_INTERFACE_DESCRIPTOR)) {
                        const auto *i=reinterpret_cast<const USB_INTERFACE_DESCRIPTOR*>(p);
                        std::printf("interface=%u alt=%u endpoints=%u class=%02x\n",i->bInterfaceNumber,i->bAlternateSetting,i->bNumEndpoints,i->bInterfaceClass);
                    } else if(p[1]==USB_ENDPOINT_DESCRIPTOR_TYPE && p[0]>=sizeof(USB_ENDPOINT_DESCRIPTOR)) {
                        const auto *e=reinterpret_cast<const USB_ENDPOINT_DESCRIPTOR*>(p);
                        std::printf("  endpoint=0x%02x type=%u max_packet=%u interval=%u\n",e->bEndpointAddress,e->bmAttributes&3,e->wMaxPacketSize,e->bInterval);
                    }
                    offset+=p[0];
                }
            }
        }
        CloseHandle(hub);
    }
    SetupDiDestroyDeviceInfoList(list);
    if(!found){std::puts("PX-BCUD was not found through USB hub descriptors.");return 2;}
    return 0;
}
