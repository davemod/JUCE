/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2020 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 6 End-User License
   Agreement and JUCE Privacy Policy (both effective as of the 16th June 2020).

   End User License Agreement: www.juce.com/juce-6-licence
   Privacy Policy: www.juce.com/juce-privacy-policy

   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

namespace
{
//==============================================================================
/** Allows a block of data to be accessed as a stream of OSC data.
 
 The memory is shared and will be neither copied nor owned by the OSCInputStream.
 
 This class is implementing the Open Sound Control 1.0 Specification for
 interpreting the data.
 
 Note: Some older implementations of OSC may omit the OSC Type Tag string
 in OSC messages. This class will treat such OSC messages as format errors.
 */
class OSCInputStream
{
public:
    /** Creates an OSCInputStream.
     
     @param sourceData               the block of data to use as the stream's source
     @param sourceDataSize           the number of bytes in the source data block
     */
    OSCInputStream (const void* sourceData, size_t sourceDataSize)
    : input (sourceData, sourceDataSize, false)
    {}
    
    //==============================================================================
    /** Returns a pointer to the source data block from which this stream is reading. */
    const void* getData() const noexcept        { return input.getData(); }
    
    /** Returns the number of bytes of source data in the block from which this stream is reading. */
    size_t getDataSize() const noexcept         { return input.getDataSize(); }
    
    /** Returns the current position of the stream. */
    uint64 getPosition()                        { return (uint64) input.getPosition(); }
    
    /** Attempts to set the current position of the stream. Returns true if this was successful. */
    bool setPosition (int64 pos)                { return input.setPosition (pos); }
    
    /** Returns the total amount of data in bytes accessible by this stream. */
    int64 getTotalLength()                      { return input.getTotalLength(); }
    
    /** Returns true if the stream has no more data to read. */
    bool isExhausted()                          { return input.isExhausted(); }
    
    //==============================================================================
    int32 readInt32()
    {
        checkBytesAvailable (4, "OSC input stream exhausted while reading int32");
        return input.readIntBigEndian();
    }
    
    uint64 readUint64()
    {
        checkBytesAvailable (8, "OSC input stream exhausted while reading uint64");
        return (uint64) input.readInt64BigEndian();
    }
    
    float readFloat32()
    {
        checkBytesAvailable (4, "OSC input stream exhausted while reading float");
        return input.readFloatBigEndian();
    }
    
    String readString()
    {
        checkBytesAvailable (4, "OSC input stream exhausted while reading string");
        
        auto posBegin = (size_t) getPosition();
        auto s = input.readString();
        auto posEnd = (size_t) getPosition();
        
        if (static_cast<const char*> (getData()) [posEnd - 1] != '\0')
            throw OSCFormatError ("OSC input stream exhausted before finding null terminator of string");
        
        size_t bytesRead = posEnd - posBegin;
        readPaddingZeros (bytesRead);
        
        return s;
    }
    
    MemoryBlock readBlob()
    {
        checkBytesAvailable (4, "OSC input stream exhausted while reading blob");
        
        auto blobDataSize = input.readIntBigEndian();
        checkBytesAvailable ((blobDataSize + 3) % 4, "OSC input stream exhausted before reaching end of blob");
        
        MemoryBlock blob;
        auto bytesRead = input.readIntoMemoryBlock (blob, (ssize_t) blobDataSize);
        readPaddingZeros (bytesRead);
        
        return blob;
    }
    
    OSCColour readColour()
    {
        checkBytesAvailable (4, "OSC input stream exhausted while reading colour");
        return OSCColour::fromInt32 ((uint32) input.readIntBigEndian());
    }
    
    OSCTimeTag readTimeTag()
    {
        checkBytesAvailable (8, "OSC input stream exhausted while reading time tag");
        return OSCTimeTag (uint64 (input.readInt64BigEndian()));
    }
    
    OSCAddress readAddress()
    {
        return OSCAddress (readString());
    }
    
    OSCAddressPattern readAddressPattern()
    {
        return OSCAddressPattern (readString());
    }
    
    //==============================================================================
    OSCTypeList readTypeTagString()
    {
        OSCTypeList typeList;
        
        checkBytesAvailable (4, "OSC input stream exhausted while reading type tag string");
        
        if (input.readByte() != ',')
            throw OSCFormatError ("OSC input stream format error: expected type tag string");
        
        for (;;)
        {
            if (isExhausted())
                throw OSCFormatError ("OSC input stream exhausted while reading type tag string");
            
            const OSCType type = input.readByte();
            
            if (type == 0)
                break;  // encountered null terminator. list is complete.
            
            if (! OSCTypes::isSupportedType (type))
                throw OSCFormatError ("OSC input stream format error: encountered unsupported type tag");
            
            typeList.add (type);
        }
        
        auto bytesRead = (size_t) typeList.size() + 2;
        readPaddingZeros (bytesRead);
        
        return typeList;
    }
    
    //==============================================================================
    OSCArgument readArgument (OSCType type)
    {
        switch (type)
        {
                
            case OSCTypes::int32:       return OSCArgument (readInt32());
            case OSCTypes::float32:     return OSCArgument (readFloat32());
            case OSCTypes::string:      return OSCArgument (readString());
            case OSCTypes::blob:        return OSCArgument (readBlob());
            case OSCTypes::colour:      return OSCArgument (readColour());
                
            default:
                // You supplied an invalid OSCType when calling readArgument! This should never happen.
                jassertfalse;
                throw OSCInternalError ("OSC input stream: internal error while reading message argument");
        }
    }
    
    //==============================================================================
    OSCMessage readMessage()
    {
        auto ap = readAddressPattern();
        auto types = readTypeTagString();
        
        OSCMessage msg (ap);
        
        for (auto& type : types)
            msg.addArgument (readArgument (type));
        
        return msg;
    }
    
    //==============================================================================
    OSCBundle readBundle (size_t maxBytesToRead = std::numeric_limits<size_t>::max())
    {
        // maxBytesToRead is only passed in here in case this bundle is a nested
        // bundle, so we know when to consider the next element *not* part of this
        // bundle anymore (but part of the outer bundle) and return.
        
        checkBytesAvailable (16, "OSC input stream exhausted while reading bundle");
        
        if (readString() != "#bundle")
            throw OSCFormatError ("OSC input stream format error: bundle does not start with string '#bundle'");
        
        OSCBundle bundle (readTimeTag());
        
        size_t bytesRead = 16; // already read "#bundle" and timeTag
        auto pos = getPosition();
        
        while (! isExhausted() && bytesRead < maxBytesToRead)
        {
            bundle.addElement (readElement());
            
            auto newPos = getPosition();
            bytesRead += (size_t) (newPos - pos);
            pos = newPos;
        }
        
        return bundle;
    }
    
    //==============================================================================
    OSCBundle::Element readElement()
    {
        checkBytesAvailable (4, "OSC input stream exhausted while reading bundle element size");
        
        auto elementSize = (size_t) readInt32();
        
        if (elementSize < 4)
            throw OSCFormatError ("OSC input stream format error: invalid bundle element size");
        
        return readElementWithKnownSize (elementSize);
    }
    
    //==============================================================================
    OSCBundle::Element readElementWithKnownSize (size_t elementSize)
    {
        checkBytesAvailable ((int64) elementSize, "OSC input stream exhausted while reading bundle element content");
        
        auto firstContentChar = static_cast<const char*> (getData()) [getPosition()];
        
        if (firstContentChar == '/')  return OSCBundle::Element (readMessageWithCheckedSize (elementSize));
        if (firstContentChar == '#')  return OSCBundle::Element (readBundleWithCheckedSize (elementSize));
        
        throw OSCFormatError ("OSC input stream: invalid bundle element content");
    }
    
private:
    MemoryInputStream input;
    
    //==============================================================================
    void readPaddingZeros (size_t bytesRead)
    {
        size_t numZeros = ~(bytesRead - 1) & 0x03;
        
        while (numZeros > 0)
        {
            if (isExhausted() || input.readByte() != 0)
                throw OSCFormatError ("OSC input stream format error: missing padding zeros");
            
            --numZeros;
        }
    }
    
    OSCBundle readBundleWithCheckedSize (size_t size)
    {
        auto begin = (size_t) getPosition();
        auto maxBytesToRead = size - 4; // we've already read 4 bytes (the bundle size)
        
        OSCBundle bundle (readBundle (maxBytesToRead));
        
        if (getPosition() - begin != size)
            throw OSCFormatError ("OSC input stream format error: wrong element content size encountered while reading");
        
        return bundle;
    }
    
    OSCMessage readMessageWithCheckedSize (size_t size)
    {
        auto begin = (size_t) getPosition();
        auto message = readMessage();
        
        if (getPosition() - begin != size)
            throw OSCFormatError ("OSC input stream format error: wrong element content size encountered while reading");
        
        return message;
    }
    
    void checkBytesAvailable (int64 requiredBytes, const char* message)
    {
        if (input.getNumBytesRemaining() < requiredBytes)
            throw OSCFormatError (message);
    }
};

} // namespace


//==============================================================================
/**
    A class for receiving OSC data.

    An OSCReceiver object allows you to receive OSC bundles and messages.
    It can connect to a network port, receive incoming OSC packets from the
    network via UDP, parse them, and forward the included OSCMessage and OSCBundle
    objects to its listeners.

    @tags{OSC}
*/
class JUCE_API  OSCReceiver
{
public:
    //==============================================================================
    /** Creates an OSCReceiver. */
    OSCReceiver();

    /** Creates an OSCReceiver with a specific name for its thread. */
    OSCReceiver (const String& threadName);

    /** Destructor. */
    ~OSCReceiver();

    //==============================================================================
    /** Connects to the specified UDP port using a datagram socket,
        and starts listening to OSC packets arriving on this port.

        @returns true if the connection was successful; false otherwise.
    */
    bool connect (int portNumber);

    /** Connects to a UDP datagram socket that is already set up,
        and starts listening to OSC packets arriving on this port.
        Make sure that the object you give it doesn't get deleted while this
        object is still using it!
        @returns true if the connection was successful; false otherwise.
    */
    bool connectToSocket (DatagramSocket& socketToUse);

    //==============================================================================
    /** Disconnects from the currently used UDP port.
        @returns true if the disconnection was successful; false otherwise.
    */
    bool disconnect();


    //==============================================================================
    /** Use this struct as the template parameter for Listener and
        ListenerWithOSCAddress to receive incoming OSC data on the message thread.
        This should be used by OSC callbacks that are not realtime-critical, but
        have significant work to do, for example updating Components in your app's
        user interface.

        This is the default type of OSC listener.
     */
    struct JUCE_API  MessageLoopCallback {};

    /** Use this struct as the template parameter for Listener and
        ListenerWithOSCAddress to receive incoming OSC data immediately after it
        arrives, called directly on the network thread that listens to incoming
        OSC traffic.
        This type can be used by OSC callbacks that don't do much, but are
        realtime-critical, for example, setting real-time audio parameters.
    */
    struct JUCE_API  RealtimeCallback {};

    //==============================================================================
    /** A class for receiving OSC data from an OSCReceiver.

        The template argument CallbackType determines how the callback will be called
        and has to be either MessageLoopCallback or RealtimeCallback. If not specified,
        MessageLoopCallback will be used by default.

        @see OSCReceiver::addListener, OSCReceiver::ListenerWithOSCAddress,
             OSCReceiver::MessageLoopCallback, OSCReceiver::RealtimeCallback

    */
    template <typename CallbackType = MessageLoopCallback>
    class JUCE_API  Listener
    {
    public:
        /** Destructor. */
        virtual ~Listener() = default;

        /** Called when the OSCReceiver receives a new OSC message.
            You must implement this function.
        */
        virtual void oscMessageReceived (const OSCMessage& message) = 0;

        /** Called when the OSCReceiver receives a new OSC bundle.
            If you are not interested in OSC bundles, just ignore this method.
            The default implementation provided here will simply do nothing.
        */
        virtual void oscBundleReceived (const OSCBundle& /*bundle*/) {}
    };

    //==============================================================================
    /** A class for receiving only those OSC messages from an OSCReceiver that match a
        given OSC address.

        Use this class if your app receives OSC messages with different address patterns
        (for example "/juce/fader1", /juce/knob2" etc.) and you want to route those to
        different objects. This class contains pre-build functionality for that OSC
        address routing, including wildcard pattern matching (e.g. "/juce/fader[0-9]").

        This class implements the concept of an "OSC Method" from the OpenSoundControl 1.0
        specification.

        The template argument CallbackType determines how the callback will be called
        and has to be either MessageLoopCallback or RealtimeCallback. If not specified,
        MessageLoopCallback will be used by default.

        Note: This type of listener will ignore OSC bundles.

        @see OSCReceiver::addListener, OSCReceiver::Listener,
             OSCReceiver::MessageLoopCallback, OSCReceiver::RealtimeCallback
    */
    template <typename CallbackType = MessageLoopCallback>
    class JUCE_API  ListenerWithOSCAddress
    {
    public:
        /** Destructor. */
        virtual ~ListenerWithOSCAddress() = default;

        /** Called when the OSCReceiver receives an OSC message with an OSC address
            pattern that matches the OSC address with which this listener was added.
        */
        virtual void oscMessageReceived (const OSCMessage& message) = 0;
    };

    //==============================================================================
    /** Adds a listener that listens to OSC messages and bundles.
        This listener will be called on the application's message loop.
    */
    void addListener (Listener<MessageLoopCallback>* listenerToAdd);

    /** Adds a listener that listens to OSC messages and bundles.
        This listener will be called in real-time directly on the network thread
        that receives OSC data.
    */
    void addListener (Listener<RealtimeCallback>* listenerToAdd);

    /** Adds a filtered listener that listens to OSC messages matching the address
        used to register the listener here.
        The listener will be called on the application's message loop.
    */
    void addListener (ListenerWithOSCAddress<MessageLoopCallback>* listenerToAdd,
                      OSCAddress addressToMatch);

    /** Adds a filtered listener that listens to OSC messages matching the address
        used to register the listener here.
        The listener will be called on the application's message loop.
     */
    void addListener (ListenerWithOSCAddress<RealtimeCallback>* listenerToAdd,
                      OSCAddress addressToMatch);

    /** Removes a previously-registered listener. */
    void removeListener (Listener<MessageLoopCallback>* listenerToRemove);

    /** Removes a previously-registered listener. */
    void removeListener (Listener<RealtimeCallback>* listenerToRemove);

    /** Removes a previously-registered listener. */
    void removeListener (ListenerWithOSCAddress<MessageLoopCallback>* listenerToRemove);

    /** Removes a previously-registered listener. */
    void removeListener (ListenerWithOSCAddress<RealtimeCallback>* listenerToRemove);

    //==============================================================================
    /** An error handler function for OSC format errors that can be called by the
        OSCReceiver.

        The arguments passed are the pointer to and the data of the buffer that
        the OSCReceiver has failed to parse.
    */
    using FormatErrorHandler = std::function<void (const char* data, int dataSize)>;

    /** Installs a custom error handler which is called in case the receiver
        encounters a stream it cannot parse as an OSC bundle or OSC message.

        By default (i.e. if you never use this method), in case of a parsing error
        nothing happens and the invalid packet is simply discarded.
    */
    void registerFormatErrorHandler (FormatErrorHandler handler);

private:
    //==============================================================================
    struct Pimpl;
    std::unique_ptr<Pimpl> pimpl;
    friend struct OSCReceiverCallbackMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OSCReceiver)
};

} // namespace juce
