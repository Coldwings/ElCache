/*
 * ElCache C SDK Example
 * 
 * This example demonstrates using the C SDK to:
 * - Connect to ElCache server
 * - Store and retrieve values
 * - Perform partial reads
 */

#include "elcache/elcache_sdk.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void print_stats(elcache_client_t* client) {
    elcache_stats_t stats;
    if (elcache_client_stats(client, &stats) == ELCACHE_OK) {
        printf("Stats:\n");
        printf("  Hits: %lu\n", (unsigned long)stats.hits);
        printf("  Misses: %lu\n", (unsigned long)stats.misses);
        printf("  Bytes read: %lu\n", (unsigned long)stats.bytes_read);
        printf("  Bytes written: %lu\n", (unsigned long)stats.bytes_written);
        printf("  Avg latency: %lu us\n", (unsigned long)stats.latency_us_avg);
    }
}

int main(int argc, char* argv[]) {
    const char* socket_path = "/var/run/elcache/elcache.sock";
    
    if (argc > 1) {
        socket_path = argv[1];
    }
    
    printf("ElCache C SDK Example\n");
    printf("=====================\n\n");
    
    /* Create client */
    elcache_client_t* client = elcache_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
    
    /* Configure client */
    elcache_client_set_timeout(client, 5000);  /* 5 second timeout */
    
    /* Try to connect */
    printf("Connecting to %s...\n", socket_path);
    elcache_error_t err = elcache_client_connect_unix(client, socket_path);
    
    if (err != ELCACHE_OK) {
        printf("Note: Could not connect to server (%s)\n", elcache_error_string(err));
        printf("This example requires a running ElCache server.\n");
        printf("\nDemonstrating API usage without server:\n\n");
        
        /* Show API usage even without connection */
        printf("1. Put operation:\n");
        printf("   elcache_client_put(client, \"key\", 3, \"value\", 5, NULL)\n\n");
        
        printf("2. Get operation:\n");
        printf("   char buffer[1024];\n");
        printf("   size_t len;\n");
        printf("   elcache_client_get(client, \"key\", 3, buffer, sizeof(buffer), &len, NULL)\n\n");
        
        printf("3. Partial read:\n");
        printf("   elcache_get_options_t opts = ELCACHE_GET_OPTIONS_INIT;\n");
        printf("   opts.offset = 100;\n");
        printf("   opts.length = 50;\n");
        printf("   elcache_client_get(client, \"key\", 3, buffer, sizeof(buffer), &len, &opts)\n\n");
        
        printf("4. Put with TTL:\n");
        printf("   elcache_put_options_t put_opts = ELCACHE_PUT_OPTIONS_INIT;\n");
        printf("   put_opts.ttl_seconds = 3600;  /* 1 hour */\n");
        printf("   elcache_client_put(client, \"key\", 3, \"value\", 5, &put_opts)\n\n");
        
        printf("5. Delete:\n");
        printf("   elcache_client_delete(client, \"key\", 3)\n\n");
        
        printf("6. Check existence:\n");
        printf("   int exists;\n");
        printf("   elcache_client_exists(client, \"key\", 3, &exists)\n\n");
        
        elcache_client_destroy(client);
        return 0;
    }
    
    printf("Connected!\n\n");
    
    /* Example: Store a simple value */
    printf("Storing key 'greeting'...\n");
    const char* key = "greeting";
    const char* value = "Hello, ElCache!";
    
    err = elcache_client_put(client, key, strlen(key), value, strlen(value), NULL);
    if (err != ELCACHE_OK) {
        fprintf(stderr, "Put failed: %s\n", elcache_error_string(err));
    } else {
        printf("Stored successfully!\n");
    }
    
    /* Example: Retrieve the value */
    printf("\nRetrieving key 'greeting'...\n");
    char buffer[1024];
    size_t len;
    
    err = elcache_client_get(client, key, strlen(key), buffer, sizeof(buffer), &len, NULL);
    if (err == ELCACHE_OK) {
        printf("Got: %.*s\n", (int)len, buffer);
    } else {
        fprintf(stderr, "Get failed: %s\n", elcache_error_string(err));
    }
    
    /* Example: Store a larger value */
    printf("\nStoring 1MB value...\n");
    char* large_value = malloc(1024 * 1024);
    if (large_value) {
        memset(large_value, 'X', 1024 * 1024);
        
        err = elcache_client_put(client, "large", 5, large_value, 1024 * 1024, NULL);
        if (err == ELCACHE_OK) {
            printf("Stored 1MB value!\n");
            
            /* Partial read */
            printf("\nReading bytes 500-600 of large value...\n");
            elcache_get_options_t opts = ELCACHE_GET_OPTIONS_INIT;
            opts.offset = 500;
            opts.length = 100;
            
            err = elcache_client_get(client, "large", 5, buffer, sizeof(buffer), &len, &opts);
            if (err == ELCACHE_OK || err == ELCACHE_ERR_PARTIAL) {
                printf("Read %zu bytes from offset 500\n", len);
            }
        }
        
        free(large_value);
    }
    
    /* Example: Delete */
    printf("\nDeleting 'greeting'...\n");
    err = elcache_client_delete(client, key, strlen(key));
    if (err == ELCACHE_OK) {
        printf("Deleted!\n");
    }
    
    /* Verify deletion */
    int exists;
    elcache_client_exists(client, key, strlen(key), &exists);
    printf("Key exists after delete: %s\n", exists ? "yes" : "no");
    
    /* Print stats */
    printf("\n");
    print_stats(client);
    
    /* Cleanup */
    elcache_client_disconnect(client);
    elcache_client_destroy(client);
    
    printf("\nExample complete!\n");
    return 0;
}
