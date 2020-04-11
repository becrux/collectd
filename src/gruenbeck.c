#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "collectd.h"
#include "plugin.h"
#include "utils/common/common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define PLUGIN_NAME "gruenbeck"
#define RUN_DIR LOCALSTATEDIR "/run"
#define HISTORY_DIR RUN_DIR "/" PLUGIN_NAME
#define HISTORY_FILE HISTORY_DIR "/history.dat"

struct MemoryBuffer
{
  char response[1048576];
  size_t size;
};

static const char *configKeys[] =
{
  "Host",
  "Retry",
};

static int configKeysNum = STATIC_ARRAY_SIZE(configKeys);

static int useHistory = 1;
static CURL *curlHandle = NULL;
static char *url = NULL;
static int retries = 1;

static int gruenbeck_config(const char *key, const char *value)
{
  if (!strncmp(key, "Host", 5)) {
    url = (char *) malloc(4096);
    strcpy(url, "http://");
    strncat(url, value, 3072);
    strcat(url, "/mux_http");

    INFO("%s: url = %s", PLUGIN_NAME, url);
  } else if (!strncmp(key, "Retry", 6)) {
    retries = atoi(value);

    INFO("%s: retries = %d", PLUGIN_NAME, retries);
  } else {
    ERROR("%s: config failed, wrong key/value pair (%s, %s)", PLUGIN_NAME, key, value);
    return -1;
  }

  /* The return value must be zero upon success, greater than zero if it failed or less than zero if key has an invalid value. */
  return 0;
}

static size_t write_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
  struct MemoryBuffer *mb = (struct MemoryBuffer *) userp;
  memcpy(mb->response + mb->size, buffer, size * nmemb);
  mb->size += size * nmemb;
  mb->response[mb->size] = '\0';
  return size * nmemb;
}

static int gruenbeck_init()
{
  mkdir(RUN_DIR, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
  mkdir(HISTORY_DIR, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP);
  if (access(HISTORY_DIR, R_OK | W_OK))
  {
    WARNING("%s: cannot create history dir %s, errno = %d, history will be disabled", PLUGIN_NAME, HISTORY_DIR, errno);
    useHistory = 0;
  }

  CURLcode curlErr;
  curlErr = curl_global_init(0);
  if (curlErr != CURLE_OK)
  {
    ERROR("%s: curl_global_init failed, %s", PLUGIN_NAME, curl_easy_strerror(curlErr));
    return -1;
  }

  curlHandle = curl_easy_init();
  if (!curlHandle)
  {
    ERROR("%s: curl_easy_init failed", PLUGIN_NAME);
    return -1;
  }

  curl_easy_setopt(curlHandle, CURLOPT_URL, url);
  if (useHistory)
    curl_easy_setopt(curlHandle, CURLOPT_POSTFIELDS, "id=625&show=D_Y_2_1|D_Y_2_2|D_Y_2_3|D_Y_2_4|D_Y_2_5|D_Y_2_6|D_Y_2_7|D_Y_2_8|D_Y_2_9|D_Y_2_10|D_Y_2_11|D_Y_2_12|D_Y_2_13|D_Y_2_14~");
  else
    curl_easy_setopt(curlHandle, CURLOPT_POSTFIELDS, "id=625&show=D_Y_2_1~");
  curl_easy_setopt(curlHandle, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, write_data);
  /* The function has to return zero if successful and non-zero otherwise. If non-zero is returned all functions the plugin registered will be unregistered. */
  return 0;
}

static int read_from_device(int *values)
{
  int errCode = -1;
  int idx = 0;
  CURLcode curlErr;
  xmlDocPtr doc = NULL;
  xmlNodePtr rootNode = NULL;
  xmlNodePtr childNode = NULL;
  const char *nodeContent = NULL;
  struct MemoryBuffer *mb = malloc(sizeof(struct MemoryBuffer));

  memset(values, 0, 14 * sizeof(int));

  mb->size = 0;

  curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, (void *) mb);

  do
  {
    curlErr = curl_easy_perform(curlHandle);
    if (curlErr == CURLE_OK)
      break;

    sleep(3);
  }
  while (--retries);

  if (curlErr != CURLE_OK)
  {
    ERROR("%s: curl_easy_perform failed, %s", PLUGIN_NAME, curl_easy_strerror(curlErr));
    goto exit;
  }

  INFO("%s: response = %s", PLUGIN_NAME, mb->response);

  doc = xmlReadMemory(mb->response, mb->size, "noname.xml", NULL, 0);

  if (doc == NULL)
  {
    ERROR("%s: xmlReadMemory failed", PLUGIN_NAME);
    goto exit;
  }

  rootNode = xmlDocGetRootElement(doc);
  if (rootNode == NULL)
  {
    ERROR("%s: xmlDocGetRootElement failed", PLUGIN_NAME);
    goto exit;
  }

  for (childNode = rootNode->children; childNode != NULL; childNode = childNode->next)
  {
    if (childNode->type == XML_ELEMENT_NODE)
    {
      if (!strncmp((const char *) childNode->name, "code", 5))
      {
        nodeContent = (const char *) xmlNodeGetContent(childNode);
        if (strncmp(nodeContent, "ok", 3))
          break;
      }
      else if (!strncmp((const char *) childNode->name, "D_Y_2_", 6))
      {
        idx = atoi((const char *) childNode->name + 6);
        nodeContent = (const char *) xmlNodeGetContent(childNode);
        values[idx - 1] = atoi(nodeContent);
        errCode = 0;

        if ((idx == 1) && !useHistory)
          break;
      }
    }
  }

exit:
  if (doc != NULL)
    xmlFree(doc);
  if (mb != NULL)
    free((void *) mb);

  return errCode;
}

static int gruenbeck_read(void)
{
  int idx = 0;
  int values[14];
  time_t lastTimestamp = 0;
  value_list_t vl = VALUE_LIST_INIT;
  FILE *historyFp = NULL;
  time_t now = time(NULL);
  struct tm now_tm;

  localtime_r(&now, &now_tm);
  if (now_tm.tm_hour < 23)
    return 0;

  now_tm.tm_sec = 0;
  now_tm.tm_min = 0;
  now_tm.tm_hour = 23;

  now = mktime(&now_tm) - 86400;

  sstrncpy(vl.plugin, "gruenbeck", sizeof(vl.plugin));
  sstrncpy(vl.type, "gauge", sizeof(vl.type));
  sstrncpy(vl.type_instance, "water", sizeof(vl.type_instance));

  if (useHistory && (!access(HISTORY_FILE, R_OK | W_OK) || (errno == ENOENT)))
  {
    historyFp = fopen(HISTORY_FILE, "rt");

    if (historyFp)
    {
      fscanf(historyFp, "%ld", &lastTimestamp);
      fclose(historyFp);
    }

    INFO("%s: last timestamp = %ld", PLUGIN_NAME, lastTimestamp);

    if (now <= lastTimestamp)
      WARNING("%s: already updated, no data sent", PLUGIN_NAME);

    if (read_from_device(values))
      return -1;

    historyFp = fopen(HISTORY_FILE, "wt");

    if (historyFp)
    {
      fprintf(historyFp, "%ld", now);
      fclose(historyFp);
    }

    now -= 86400 * 13;
    for (idx = 13; idx >= 0; --idx, now += 86400)
    {
      if (now > lastTimestamp)
      {
        vl.values = &(value_t){.gauge = values[idx]};
        vl.values_len = 1;
        vl.time = TIME_T_TO_CDTIME_T(now);

        INFO("%s: send data, %ld => %d", PLUGIN_NAME, now, values[idx]);

        plugin_dispatch_values(&vl);
      }
    }
  }
  else
  {
    if (read_from_device(values))
      return -1;

    vl.values = &(value_t){.gauge = values[0]};
    vl.values_len = 1;
    vl.time = TIME_T_TO_CDTIME_T(now);

    INFO("%s: send data, %ld => %d", PLUGIN_NAME, now, values[0]);

    plugin_dispatch_values(&vl);
  }

  return 0;
  /* The function has to return zero if successful and non-zero otherwise. */
  /* If non-zero is returned, the value will be suspended for an increasing interval, but no longer than 86,400 seconds (one day). */
}
  
static int gruenbeck_shutdown()
{
  if (url != NULL)
    free((void *) url);

  curl_easy_cleanup(curlHandle);
  curl_global_cleanup();
  xmlCleanupParser();
  return 0;
  /* The function has to return zero if successful and non-zero otherwise. */
}
  
void module_register()
{       
  plugin_register_config(PLUGIN_NAME, gruenbeck_config, configKeys, configKeysNum);
  plugin_register_init(PLUGIN_NAME, gruenbeck_init);
  plugin_register_read(PLUGIN_NAME, gruenbeck_read);
  plugin_register_shutdown(PLUGIN_NAME, gruenbeck_shutdown);
}
