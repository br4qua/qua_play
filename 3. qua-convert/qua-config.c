#include "qua-config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Check if value is in space-separated list
bool qua_config_is_valid(const char *valid_list, int value) {
  if (!valid_list || valid_list[0] == '\0')
    return false;
  
  char list_copy[256];
  strncpy(list_copy, valid_list, sizeof(list_copy) - 1);
  list_copy[sizeof(list_copy) - 1] = '\0';
  
  char *token = strtok(list_copy, " ");
  while (token) {
    int valid_value = atoi(token);
    if (valid_value == value)
      return true;
    token = strtok(NULL, " ");
  }
  
  return false;
}

// Get target bit depth: override -> detected (if valid) -> fallback
int qua_config_get_target_bit_depth(int detected_bd) {
  // Check override first
  if (BIT_DEPTH_OVERRIDE[0] != '\0') {
    int override = atoi(BIT_DEPTH_OVERRIDE);
    if (override > 0)
      return override;
  }
  
  // Check if detected is valid
  if (qua_config_is_valid(BIT_DEPTH_VALID, detected_bd)){
    return detected_bd;
  }
  // Use fallback
  return BIT_DEPTH_FALLBACK;
}

// Get target sample rate: override -> detected (if valid) -> fallback
int qua_config_get_target_sample_rate(int detected_sr) {
  // Check override first
  if (SAMPLE_RATE_OVERRIDE[0] != '\0') {
    int override = atoi(SAMPLE_RATE_OVERRIDE);
    if (override > 0)
      return override;
  }
  
  // Check if detected is valid
  if (qua_config_is_valid(SAMPLE_RATE_VALID, detected_sr))
    return detected_sr;
  
  // Use fallback
  return SAMPLE_RATE_FALLBACK;
}
