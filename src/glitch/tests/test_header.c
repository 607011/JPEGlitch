#include <string.h>

#include "jpeglitch_core.h"
#include "test_util.h"

int main(void)
{
	/* SOS marker present somewhere in the middle. */
	uint8_t with_marker[] = {0x00, 0x01, 0xff, 0xda, 0x05, 0x06, 0x07};
	CHECK(jpeglitch_find_header_size(with_marker, sizeof(with_marker)) == 4);

	/* SOS marker at the very start. */
	uint8_t at_start[] = {0xff, 0xda, 0x01, 0x02};
	CHECK(jpeglitch_find_header_size(at_start, sizeof(at_start)) == 2);

	/* No marker at all -> headerSize = 0. */
	uint8_t no_marker[] = {0x00, 0x01, 0x02, 0xff, 0xd8, 0x03};
	CHECK(jpeglitch_find_header_size(no_marker, sizeof(no_marker)) == 0);

	/* An empty or 1-byte buffer never finds a 2-byte marker. */
	uint8_t one_byte[] = {0xff};
	CHECK(jpeglitch_find_header_size(one_byte, sizeof(one_byte)) == 0);
	CHECK(jpeglitch_find_header_size(NULL, 0) == 0);

	/* Only the *first* occurrence counts. */
	uint8_t two_markers[] = {0xff, 0xda, 0x01, 0xff, 0xda, 0x02};
	CHECK(jpeglitch_find_header_size(two_markers, sizeof(two_markers)) == 2);

	TEST_EXIT();
}
