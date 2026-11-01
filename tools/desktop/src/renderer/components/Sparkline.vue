<script setup lang="ts">
import { computed } from "vue";
const props = withDefaults(
  defineProps<{
    values: (number | null)[];
    color?: string;
    label?: string;
    height?: number;
  }>(),
  { color: "#4d81f6", height: 130, label: "趋势图" },
);
const points = computed(() => {
  const values = props.values.filter(
    (value): value is number => value !== null && Number.isFinite(value),
  );
  if (values.length < 2) return "";
  const maximum = Math.max(...values, 1);
  const minimum = Math.min(...values, 0);
  return values
    .map(
      (value, index) =>
        `${(index / (values.length - 1)) * 600},${props.height - 12 - ((value - minimum) / (maximum - minimum || 1)) * (props.height - 24)}`,
    )
    .join(" ");
});
</script>

<template>
  <div
    class="sparkline"
    :style="{ height: height + 'px', '--chart-color': color }"
  >
    <svg
      v-if="points"
      preserveAspectRatio="none"
      role="img"
      :aria-label="label"
      :style="{ height: height + 'px' }"
      :viewBox="`0 0 600 ${height}`"
    >
      <line
        v-for="fraction in [0.25, 0.5, 0.75]"
        :key="fraction"
        x1="0"
        x2="600"
        :y1="height * fraction"
        :y2="height * fraction"
        class="chart-grid"
      />
      <polygon
        :points="`0,${height} ${points} 600,${height}`"
        :fill="color"
        opacity=".07"
      />
      <polyline
        :points="points"
        :stroke="color"
        fill="none"
        stroke-width="2.5"
        vector-effect="non-scaling-stroke"
        stroke-linejoin="round"
      />
    </svg>
    <div v-else class="chart-waiting">
      <span class="pulse-dot" />等待更多采样点
    </div>
  </div>
</template>
