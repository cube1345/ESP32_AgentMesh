import axios from 'axios';
import { mockDashboardPayload } from '../data/mock';
import type { DashboardPayload, SkillDraft, UserPreferenceProfile } from '../types';

const api = axios.create({
  baseURL: '/api',
  timeout: 2000
});

function clonePayload(): DashboardPayload {
  return JSON.parse(JSON.stringify(mockDashboardPayload)) as DashboardPayload;
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function isDashboardPayload(value: unknown): value is DashboardPayload {
  if (!isObject(value)) {
    return false;
  }

  return (
    Array.isArray(value.nodes) &&
    Array.isArray(value.capabilities) &&
    Array.isArray(value.timeline) &&
    Array.isArray(value.environment) &&
    Array.isArray(value.skills) &&
    isObject(value.preferences)
  );
}

function isSkillArray(value: unknown): value is SkillDraft[] {
  return Array.isArray(value);
}

function isPreferenceProfile(value: unknown): value is UserPreferenceProfile {
  return isObject(value);
}

export async function fetchDashboard(): Promise<DashboardPayload> {
  try {
    const response = await api.get<DashboardPayload>('/dashboard');
    return isDashboardPayload(response.data) ? response.data : clonePayload();
  } catch {
    return clonePayload();
  }
}

export async function saveSkills(skills: SkillDraft[]): Promise<SkillDraft[]> {
  try {
    const response = await api.post<SkillDraft[]>('/skills', { skills });
    return isSkillArray(response.data)
      ? response.data
      : JSON.parse(JSON.stringify(skills)) as SkillDraft[];
  } catch {
    return JSON.parse(JSON.stringify(skills)) as SkillDraft[];
  }
}

export async function savePreferences(
  preferences: UserPreferenceProfile
): Promise<UserPreferenceProfile> {
  try {
    const response = await api.post<UserPreferenceProfile>('/preferences', preferences);
    return isPreferenceProfile(response.data)
      ? response.data
      : JSON.parse(JSON.stringify(preferences)) as UserPreferenceProfile;
  } catch {
    return JSON.parse(JSON.stringify(preferences)) as UserPreferenceProfile;
  }
}
