#include "fcs16.h"
#include "yahdlc.h"

void yahdlc_escape_value(char value, char *dest, int *dest_index) {
  // Check if the value should be escaped
  if ((value == YAHDLC_FLAG_SEQUENCE) || (value == YAHDLC_CONTROL_ESCAPE)) {
    dest[*dest_index] = YAHDLC_CONTROL_ESCAPE;
    *dest_index += 1;
    value = value ^ 0x20;
  }

  dest[*dest_index] = value;
  *dest_index += 1;
}

struct yahdlc_control_t yahdlc_get_control_value(unsigned char control) {
  struct yahdlc_control_t value;

  // Check if the frame is a S-frame
  if (control & 0x1) {
    // Check if S-frame is an ACK (Receive Ready S-frame)
    if ((control & 0xF) == 0x1) {
      value.frame = YAHDLC_FRAME_ACK;
    } else {
      // Assume it is an NACK since Receive Not Ready, Selective Reject and U-frames are not supported
      value.frame = YAHDLC_FRAME_NACK;
    }
    // Just clear the send sequence number as it is not part of an S-frame (and U-frame)
    value.send_seq_no = 0;
  } else {
    // It must be an I-frame so add the send sequence number
    value.frame = YAHDLC_FRAME_DATA;
    value.send_seq_no = ((control >> 1) & 0x7);
  }

  // The receive sequence number is present in all frames
  value.recv_seq_no = ((control >> 5) & 0x7);

  return value;
}

unsigned char yahdlc_frame_control_value(struct yahdlc_control_t control) {
  unsigned char value = 0;

  // For details see: https://en.wikipedia.org/wiki/High-Level_Data_Link_Control
  switch (control.frame) {
    case YAHDLC_FRAME_DATA:
      // Create the HDLC I-frame control byte with poll bit set
      value |= ((control.recv_seq_no & 0x7) << 5);
      value |= (1 << 4);  // Set poll bit
      value |= ((control.send_seq_no & 0x7) << 1);
      break;
    case YAHDLC_FRAME_ACK:
      // Create the HDLC Receive Ready S-frame control byte with poll bit cleared (final)
      value |= ((control.recv_seq_no & 0x7) << 5);
      value |= 1;  // Set S-frame bit
      break;
    case YAHDLC_FRAME_NACK:
      // Create the HDLC Receive Ready S-frame control byte with poll bit cleared (final)
      value |= ((control.recv_seq_no & 0x7) << 5);
      value |= (1 << 3);  // Reject S-frame
      value |= 1;  // Set S-frame bit
      break;
  }

  return value;
}

int yahdlc_get_data(struct yahdlc_control_t *control, const char *src,
                    unsigned int src_len, char *dest, unsigned int *dest_len) {
  unsigned int i;
  static unsigned char control_escape = 0, value;
  static unsigned short fcs = FCS16_INIT_VALUE;
  static int ret, start_index = -1, end_index = -1, src_index = 0, dest_index =
      0;

  // Run through the data
  for (i = 0; i < src_len; i++) {
    // First find the start flag sequence
    if (start_index < 0) {
      if (src[i] == YAHDLC_FLAG_SEQUENCE) {
        // Check if an additional flag sequence byte is present
        if ((i < (src_len - 1)) && (src[i + 1] == YAHDLC_FLAG_SEQUENCE)) {
          // Just loop again to silently discard it (accordingly to HDLC)
          continue;
        }

        start_index = src_index;
      }
    } else {
      // Check for end flag sequence
      if (src[i] == YAHDLC_FLAG_SEQUENCE) {
        // Check if an additional flag sequence byte is present or was earlier received
        if (((i < (src_len - 1)) && (src[i + 1] == YAHDLC_FLAG_SEQUENCE))
            || ((start_index + 1) == src_index)) {
          // Just loop again to silently discard it (accordingly to HDLC)
          continue;
        }

        end_index = src_index;
        break;
      } else if (src[i] == YAHDLC_CONTROL_ESCAPE) {
        control_escape = 1;
      } else {
        // Update the value based on any control escape received
        if (control_escape) {
          control_escape = 0;
          value = src[i] ^ 0x20;
        } else {
          value = src[i];
        }

        // Now update the FCS value
        fcs = fcs16(fcs, value);

        // Retrieve the control field value
        if (src_index == start_index + 2) {
          *control = yahdlc_get_control_value(value);
        }

        // Start adding the data values to the destination buffer
        if (src_index > (start_index + 2)) {
          dest[dest_index++] = value;
        }
      }
    }
    src_index++;
  }

  // Check for invalid frame (no start or end flag sequence) and FCS error (less than 4 bytes frame)
  if ((start_index < 0) || (end_index < 0)) {
    // Return no start or end flag sequence and make sure destination length is 0
    *dest_len = 0;
    ret = -1;
  } else if ((end_index < (start_index + 4)) || (fcs != FCS16_GOOD_VALUE)) {
    // Return FCS error and indicate that data up to end flag sequence in buffer should be discarded
    *dest_len = i;
    ret = -2;
  } else {
    // Return success and indicate that data up to end flag sequence in buffer should be discarded
    *dest_len = dest_index - 2;
    ret = i;
  }

  // Reset values for next frame if start and end flag sequence has been detected
  if (ret != -1) {
    fcs = FCS16_INIT_VALUE;
    src_index = dest_index = 0;
    start_index = end_index = -1;
  }

  return ret;
}

void yahdlc_frame_data(struct yahdlc_control_t control, const char *src,
                       unsigned int src_len, char *dest, unsigned int *dest_len) {
  unsigned int i;
  int dest_index = 0;
  unsigned char value;
  unsigned short fcs = FCS16_INIT_VALUE;

  // Start by adding the start flag sequence
  dest[dest_index++] = YAHDLC_FLAG_SEQUENCE;

  // Add the all station address from HDLC
  fcs = fcs16(fcs, YAHDLC_ALL_STATION);
  yahdlc_escape_value(YAHDLC_ALL_STATION, dest, &dest_index);

  // Add the converted control field
  value = yahdlc_frame_control_value(control);
  fcs = fcs16(fcs, value);
  yahdlc_escape_value(value, dest, &dest_index);

  // Calculate FCS and escape data
  for (i = 0; i < src_len; i++) {
    fcs = fcs16(fcs, src[i]);
    yahdlc_escape_value(src[i], dest, &dest_index);
  }

  // Invert the FCS value
  fcs ^= 0xFFFF;

  // Run through the FCS bytes and escape the values
  for (i = 0; i < sizeof(fcs); i++) {
    value = ((fcs >> (8 * i)) & 0xFF);
    yahdlc_escape_value(value, dest, &dest_index);
  }

  // Add end flag sequence and update length of frame
  dest[dest_index++] = YAHDLC_FLAG_SEQUENCE;
  *dest_len = dest_index;
}