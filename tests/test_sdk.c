/* SDK test stub - requires running server */

#include "elcache/elcache_sdk.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("ElCache SDK Tests\n");
    printf("=================\n\n");
    
    /* Test 1: Client creation */
    printf("Test 1: Client creation... ");
    elcache_client_t* client = elcache_client_create();
    assert(client != NULL);
    printf("PASS\n");
    
    /* Test 2: Not connected initially */
    printf("Test 2: Not connected initially... ");
    assert(!elcache_client_is_connected(client));
    printf("PASS\n");
    
    /* Test 3: Error strings */
    printf("Test 3: Error strings... ");
    assert(strcmp(elcache_error_string(ELCACHE_OK), "Success") == 0);
    assert(strcmp(elcache_error_string(ELCACHE_ERR_NOT_FOUND), "Not found") == 0);
    printf("PASS\n");
    
    /* Test 4: Invalid operations when not connected */
    printf("Test 4: Invalid ops when disconnected... ");
    char buffer[100];
    size_t len;
    elcache_error_t err = elcache_client_get(client, "key", 3, buffer, sizeof(buffer), &len, NULL);
    assert(err == ELCACHE_ERR_CONNECTION);
    printf("PASS\n");
    
    /* Test 5: Statistics */
    printf("Test 5: Statistics... ");
    elcache_stats_t stats;
    err = elcache_client_stats(client, &stats);
    assert(err == ELCACHE_OK);
    assert(stats.hits == 0);
    assert(stats.misses == 0);
    printf("PASS\n");
    
    /* Test 6: Configuration */
    printf("Test 6: Configuration... ");
    elcache_client_set_timeout(client, 5000);
    elcache_client_set_recv_buffer(client, 1024 * 1024);
    printf("PASS\n");
    
    /* Test 7: Cleanup */
    printf("Test 7: Cleanup... ");
    elcache_client_destroy(client);
    printf("PASS\n");
    
    printf("\nAll SDK tests passed!\n");
    
    /* Note: Integration tests with actual server would go here
       They would require:
       1. Starting ElCache server
       2. Connecting client
       3. Testing put/get/delete operations
       4. Testing partial reads
       5. Testing streaming operations
    */
    
    return 0;
}
