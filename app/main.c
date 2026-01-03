#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>

/* --- CONFIGURATION --- */
#define DEFAULT_DOMAIN "camera.example.com"
#define DEFAULT_INTERVAL_HOURS 24
#define DEFAULT_RETRY_SEC 300
#define UACME_DIR "/etc/ssl/uacme/private"
#define TEMP_CERT_PATH "/tmp/acme_combined.pem"

/* Global flag for signal handling */
volatile sig_atomic_t keep_running = 1;

void handle_signal(int signal) {
    keep_running = 0;
}

struct MemoryStruct {
  char *memory;
  size_t size;
};

/* Libcurl write callback */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;
  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if(!ptr) return 0;
  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
  return realsize;
}

/* Helper: Check file modification time */
time_t get_file_mtime(const char *path) {
    struct stat attr;
    if (stat(path, &attr) == 0) {
        return attr.st_mtime;
    }
    return 0;
}

/* Helper: Combine Key and Cert */
int combine_pem_files(const char *domain) {
    char cert_path[512], key_path[512];
    snprintf(cert_path, sizeof(cert_path), "%s/%s/cert.pem", UACME_DIR, domain);
    snprintf(key_path, sizeof(key_path), "%s/%s/key.pem", UACME_DIR, domain);

    FILE *f_cert = fopen(cert_path, "r");
    FILE *f_key = fopen(key_path, "r");
    FILE *f_out = fopen(TEMP_CERT_PATH, "w");

    // Check if files exist
    if (!f_cert || !f_key || !f_out) {
        if(f_cert) fclose(f_cert);
        if(f_key) fclose(f_key);
        if(f_out) fclose(f_out);
        return 0; 
    }

    char ch;
    // Write Key first
    while ((ch = fgetc(f_key)) != EOF) fputc(ch, f_out);
    fputc('\n', f_out);
    // Write Cert
    while ((ch = fgetc(f_cert)) != EOF) fputc(ch, f_out);

    fclose(f_cert);
    fclose(f_key);
    fclose(f_out);
    return 1;
}

/* VAPIX Interaction */
int update_vapix(const char *password) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;
    int success = 0;

    curl = curl_easy_init();
    if (!curl) {
        free(chunk.memory);
        return 0;
    }

    // 1. UPLOAD
    curl_mime *form = curl_mime_init(curl);
    curl_mimepart *field = curl_mime_addpart(form);
    curl_mime_name(field, "pack");
    curl_mime_filedata(field, TEMP_CERT_PATH);

    char userpwd[256];
    // Safety: truncating password if absurdly long to prevent overflow
    snprintf(userpwd, sizeof(userpwd), "root:%.200s", password);

    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1/axis-cgi/certificates/info.cgi?action=upload&type=server");
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    
    // Set a timeout for the request (10 seconds)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    res = curl_easy_perform(curl);
    
    if(res != CURLE_OK) {
        fprintf(stderr, "[ERROR] VAPIX Upload failed: %s\n", curl_easy_strerror(res));
        goto cleanup;
    }

    // 2. PARSE ID
    // Robust search: look for first occurrence of <id> and </id>
    char *id_start = strstr(chunk.memory, "<id>");
    char *id_end = strstr(chunk.memory, "</id>");
    char cert_id[128] = {0};

    if (id_start && id_end && (id_end > id_start)) {
        id_start += 4; 
        size_t len = id_end - id_start;
        if (len < sizeof(cert_id) - 1) {
            strncpy(cert_id, id_start, len);
            cert_id[len] = '\0';
        }
    } else {
        printf("[ERROR] Failed to parse Cert ID from response.\n");
        goto cleanup;
    }
    
    printf("[INFO] Certificate ID obtained: %s\n", cert_id);

    // 3. ACTIVATE
    // Reuse curl handle, reset options
    curl_easy_reset(curl);
    free(chunk.memory); // Clear previous response
    chunk.memory = malloc(1);
    chunk.size = 0;

    char url[512];
    snprintf(url, sizeof(url), "http://127.0.0.1/axis-cgi/param.cgi?action=update&Network.HTTPS.SrvID=%s", cert_id);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    res = curl_easy_perform(curl);

    if(res == CURLE_OK) {
        // VAPIX usually returns "OK" in body on success
        if(strstr(chunk.memory, "OK") != NULL || strstr(chunk.memory, "Success") != NULL) {
             printf("[INFO] HTTPS Certificate activated successfully.\n");
             success = 1;
        } else {
             // Sometimes it just returns empty or generic XML, treat as success if HTTP 200
             long http_code = 0;
             curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
             if (http_code == 200) success = 1;
        }
    } else {
        fprintf(stderr, "[ERROR] Activation request failed: %s\n", curl_easy_strerror(res));
    }

cleanup:
    curl_mime_free(form);
    curl_easy_cleanup(curl);
    free(chunk.memory);
    return success;
}

int main() {
    // Register Signal Handler for graceful container shutdown
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    // One-time global init
    curl_global_init(CURL_GLOBAL_ALL);

    char *env_domain = getenv("DOMAIN_NAME");
    char *env_pass = getenv("CAMERA_PASSWORD");
    char *env_interval = getenv("RENEW_INTERVAL_HOURS");
    char *env_retry = getenv("RETRY_WAIT_SECONDS");

    const char *domain = env_domain ? env_domain : DEFAULT_DOMAIN;
    const char *pass = env_pass ? env_pass : "pass"; 
    int interval_sec = (env_interval ? atoi(env_interval) : DEFAULT_INTERVAL_HOURS) * 3600;
    int retry_sec = env_retry ? atoi(env_retry) : DEFAULT_RETRY_SEC;
    
    // Path to the cert file we want to monitor
    char cert_check_path[512];
    snprintf(cert_check_path, sizeof(cert_check_path), "%s/%s/cert.pem", UACME_DIR, domain);

    printf("ACME Daemon Started. Domain: %s\n", domain);

    while(keep_running) {
        printf("------------------------------------------------\n");
        printf("Running Renewal Check at %ld...\n", time(NULL));

        // Snapshot modification time BEFORE running uacme
        time_t pre_mtime = get_file_mtime(cert_check_path);

        // Run uacme
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "uacme -k %s -h /app/hook.sh %s", UACME_DIR, domain);
        int status = system(cmd);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            
            // Check if cert file actually changed
            time_t post_mtime = get_file_mtime(cert_check_path);

            if (post_mtime > pre_mtime || pre_mtime == 0) {
                printf("[INFO] New certificate detected (MTime changed). updating VAPIX...\n");
                
                if (combine_pem_files(domain)) {
                    if (update_vapix(pass)) {
                        printf("[SUCCESS] Update complete. Sleeping %d sec.\n", interval_sec);
                        unlink(TEMP_CERT_PATH); 
                        
                        // Interruptible sleep (using loop for signal responsiveness)
                        for(int i=0; i < interval_sec && keep_running; i++) sleep(1);
                        continue;
                    }
                } else {
                    printf("[ERROR] Failed to combine PEM files.\n");
                }
            } else {
                printf("[INFO] Certificate is still valid (No change). Skipping VAPIX update. Retrying in %d seconds.\n");
            }
        } else {
            printf("[ERROR] uacme failed. Retrying in %d seconds.\n", retry_sec);
        }

        // Retry wait
        for(int i=0; i < retry_sec && keep_running; i++) sleep(1);
    }

    printf("Daemon shutting down.\n");
    curl_global_cleanup();
    return 0;
}
