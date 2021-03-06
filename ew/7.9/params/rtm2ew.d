# @(#)rtm2ew.d	1.0 08/01/13
# rtm2ew parameter file

#  Basic Earthworm setup:

MyModuleId         MOD_RTM2EW     # module id for this process
WaveRing           WAVE_RING      # output ring for TRACEBUF2 messages
HeartBeatInterval  30             # seconds between heartbeats

# At least one of WaveRing and RawRing must be defined.
#
# If you have selected a WaveRing to output TRACEBUF2 messages, then it
# is necessary to define a mapping between the digitizer, stream, channel
# information that is contained in each packet to the station, channel,
# net, location and pin number that is required by the TRACEBUF2 messages.  
# There is no standard way to do this in vanilla Earthworm, therefore reftek2ew 
# defines yet another parameter file.  If you don't specify this file,
# then reftek2ew issues a warning message on start up and then generates
# its own names.  Station name is the unit id, channel name is built up
# from the channel value in the DT header (Cxx where xx=chan), the
# networm name is built from the stream number in the DT header (Sx),
# the location code is set to "--", and the pin number is set to -1.

SCNLFile             /Users/scott_mbp/Documents/EW_reftek/in/reftek.scnl

# Set a flag to control whether channels not listed in the SCNLFile are
# written to the WaveRing.  Zero means unlisted channels will not be
# written to WaveRing, non-zero means they will be written using the 
# SCNL generated from the DAS packets as described above.
#### PLEASE READ THIS SECTION BEFORE SETTING SendUnknownChan ######
# The program has two modes of operation:
#     1) Track only the data for channels listed in the SCNLFile
#     2) Track all channels for which data is received.
#
#  Under mode 1, memory will be allocated to track the channels
#  when the SCNL file is read.  Once the channel list is read
#  from the file, no further data allocation, or list sorting
#  will be done, but data from all channels that are 
#  NOT IN THE SCNL LIST, will be dropped.
#
#  Under mode 2, memory will be allocated to track any channels
#  that are encountered by the module.  Each time a new channel
#  is encountered, it will be added to the list, and the list
#  will be resorted.  Channel list allocation is done in blocks,
#  and it is possible that if a new channel was added to the
#  input stream while the program was running, that it could
#  require the program to:
#              a) allocate a new large block of memory
#              b) copy the list from one memory block to another
#              c) free an old block of memory
#              d) resort the list
#  all before any other packets could be processed.
#  This would probably not be the end of the world, but does
#  add another degree of volatility to the program.
#
#  Mode 1 essentially requires the operator to predefine all
#   of the desired channels in the SCNLList, but is very stable.
#  Mode 2 is more flexible, but has more volatility.
#  Set SendUnknownChan to 0 for Mode 1, and to 1 for Mode 2.

SendUnknownChan     0


# AcceptableSampleRates 
# The program will attempt to determine the nominal sample rate for each 
# channel.  The channel rate will only be set if it matches a valid
# sample rate as specified in the config file.
# So if reftek2ew starts up, and the first 3 packets it sees from
# a channel have respective rates of 125.9, 40.0, 40.0
# it will set the sample rate to 40.0.
# Default: AcceptableSampleRates 40.0 80.0 100.0 200.0

AcceptableSampleRates 40 80 100 200


# SendTimeTearPackets
# ReftekEW observes the starttime of the packets coming from
# the reftek data stream.  If there is a gap between the "estimated" end
# of the previous packet, and the start of the current packet, then some
# sort of time tear occurred.  It is not certain where the time tear
# occurred and what if any data is corrupted.  Theoretically, the data
# in the previous packet could be invalid, the data in the current packet
# could be invalid, or both, or neither.  Because packets may be quite
# long (8+ seconds), you may wish to risk validity of some data in order
# to have access to all of the data in the packet.  The default behavior
# is to SEND the packets around a time tear.  Set to 0 to NOT SEND the
# packet after a time tear.  The packet previous to the time tear is
# ALWAYS SENT.
# Default: SendTimeTearPackets 1

SendTimeTearPackets 1


# Give the fully qualified host name, or the IP address of the server in
# the usual decimal dot notation, followed by the port number for the RTP
# service.  If you are running this on the same computer as the RTP server,
# then specify "localhost" as the host name.

Server             69.15.146.174 2543

# The API allows you to request data from a single, specific, DAS or from
# all DAS(es).  There is no provision in the API for requesting data from
# a subset of the available digitizers.  If you want data from a single DAS,
# give its unit id here, otherwise enter 0 to select all digitizers.

DASid              0


# The maximum amount of time (in seconds) before the module will abandon 
# filling a gap (but will continue to wait to fill newer gaps)

MaxGapLatency      0.5
