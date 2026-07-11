import axios from 'axios';
import type { AxiosError } from 'axios';
import { mockDashboardPayload } from '../data/mock';
import type {
  DashboardPayload,
  RuntimeSkillInstallResponse,
  RuntimeSkillListResponse,
  SkillDraft,
  UserPreferenceProfile
} from '../types';

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

function isRuntimeSkillListResponse(value: unknown): value is RuntimeSkillListResponse {
  return isObject(value) && Array.isArray(value.skills);
}

function isRuntimeSkillInstallResponse(value: unknown): value is RuntimeSkillInstallResponse {
  return isObject(value) && typeof value.ok === 'boolean';
}

function isPreferenceProfile(value: unknown): value is UserPreferenceProfile {
  return isObject(value);
}

function errorMessage(error: unknown): string {
  const axiosError = error as AxiosError<{ error?: string; message?: string }>;
  return axiosError.response?.data?.error ||
    axiosError.response?.data?.message ||
    (error instanceof Error ? error.message : 'request failed');
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
    const response = await api.post<SkillDraft[] | { skills?: SkillDraft[] }>('/skills', { skills });
    const payload = response.data;
    if (isSkillArray(payload)) {
      return payload;
    }
    return isObject(payload) && isSkillArray(payload.skills)
      ? payload.skills
      : JSON.parse(JSON.stringify(skills)) as SkillDraft[];
  } catch {
    return JSON.parse(JSON.stringify(skills)) as SkillDraft[];
  }
}

export async function fetchRuntimeSkills(): Promise<RuntimeSkillListResponse> {
  try {
    const response = await api.get<RuntimeSkillListResponse>('/skills/runtime', {
      timeout: 25000
    });
    return isRuntimeSkillListResponse(response.data)
      ? response.data
      : { skills: [], source: 'local_mock' };
  } catch {
    return { skills: [], source: 'local_mock' };
  }
}

export async function installRuntimeSkill(
  skill: SkillDraft,
  confirmed = true
): Promise<RuntimeSkillInstallResponse> {
  try {
    const response = await api.post<RuntimeSkillInstallResponse>('/skills/install', {
      skill,
      confirmed
    }, {
      timeout: 30000
    });
    return isRuntimeSkillInstallResponse(response.data)
      ? response.data
      : {
          ok: false,
          source: 'local_mock',
          message: 'invalid install response',
          error: 'invalid install response'
        };
  } catch (error) {
    return {
      ok: false,
      source: 'local_mock',
      message: 'install request failed',
      error: errorMessage(error)
    };
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
