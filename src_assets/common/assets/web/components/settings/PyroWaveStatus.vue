<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet } from '@/api/client';
import { AppButton, StatusBadge } from '@/components/ui';

interface Capabilities {
  compiled: boolean;
  enabled: boolean;
  available: boolean;
  reason: string;
  adapter: string;
}

const { t } = useI18n();
const capability = ref<Capabilities | null>(null);
const loading = ref(false);
const failed = ref(false);

async function refresh() {
  if (loading.value) return;
  loading.value = true;
  failed.value = false;
  try {
    capability.value = await apiGet<Capabilities>('/api/pyrowave/capabilities');
  } catch {
    capability.value = null;
    failed.value = true;
  } finally {
    loading.value = false;
  }
}

onMounted(() => void refresh());
</script>

<template>
  <section class="pyrowave-status" :aria-label="t('config.pyrowave_status_label')">
    <div class="pyrowave-status__heading">
      <StatusBadge v-if="capability" :tone="capability.available ? 'success' : 'neutral'">
        {{
          t(
            capability.available
              ? 'config.pyrowave_status_ready'
              : !capability.compiled
                ? 'config.pyrowave_status_not_built'
                : !capability.enabled
                  ? 'config.pyrowave_status_disabled'
                  : 'config.pyrowave_status_unavailable',
          )
        }}
      </StatusBadge>
      <AppButton size="compact" :busy="loading" @click="refresh">
        {{ t('config.pyrowave_status_check') }}
      </AppButton>
    </div>
    <p>{{ t('config.pyrowave_status_saved') }}</p>
    <p v-if="failed" role="alert">{{ t('config.pyrowave_status_failed') }}</p>
    <p v-else-if="capability?.reason && capability.compiled && capability.enabled" role="status">
      {{ capability.reason }}
    </p>
    <p v-if="capability?.available">{{ t('config.pyrowave_status_scope') }}</p>
  </section>
</template>

<style scoped>
.pyrowave-status {
  display: grid;
  gap: 0.5rem;
  margin-top: 1rem;
}
.pyrowave-status__heading {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 0.75rem;
}
.pyrowave-status p {
  margin: 0;
  font-size: 0.875rem;
}
</style>
