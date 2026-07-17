import axios from 'axios';
import type { AxiosError } from 'axios';
import { mockDashboardPayload } from '../data/mock';
import type {
  DashboardPayload,
  RuntimeDeviceManifestListResponse,
  RuntimeDeviceManifestRecord,
  RuntimeDeviceManifestUpdateResponse,
  RuntimeSkillDetailResponse,
  RuntimeSkillInstallResponse,
  RuntimeSkillListResponse,
  RuntimeSkillReadResult,
  RuntimeSkillRecord,
  SkillDraft,
  UserPreferenceProfile
} from '../types';

const api = axios.create({
  baseURL: '/api',
  timeout: 2000
});

const RUNTIME_LIST_TIMEOUT_MS = 45000;
const RUNTIME_INSTALL_TIMEOUT_MS = 90000;

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
    Array.isArray(value.environmentHistory) &&
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

function isRuntimeSkillDetailResponse(value: unknown): value is RuntimeSkillDetailResponse {
  return isObject(value) && isObject(value.skill) && typeof value.source === 'string';
}

function isRuntimeDeviceManifestListResponse(value: unknown): value is RuntimeDeviceManifestListResponse {
  return isObject(value) && Array.isArray(value.devices);
}

function isRuntimeDeviceManifestUpdateResponse(value: unknown): value is RuntimeDeviceManifestUpdateResponse {
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
      timeout: RUNTIME_LIST_TIMEOUT_MS
    });
    return isRuntimeSkillListResponse(response.data)
      ? response.data
      : { skills: [], source: 'local_mock' };
  } catch (error) {
    return { skills: [], source: 'local_mock', error: errorMessage(error) };
  }
}

export async function fetchRuntimeSkill(runtimeName: string): Promise<RuntimeSkillReadResult> {
  try {
    const response = await api.get<RuntimeSkillDetailResponse>('/skills/runtime', {
      params: { name: runtimeName },
      timeout: RUNTIME_LIST_TIMEOUT_MS
    });
    return isRuntimeSkillDetailResponse(response.data)
      ? { skill: response.data.skill, source: response.data.source }
      : { skill: null, source: 'local_mock', error: 'invalid runtime skill detail response' };
  } catch (error) {
    return { skill: null, source: 'local_mock', error: errorMessage(error) };
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
      timeout: RUNTIME_INSTALL_TIMEOUT_MS
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

export async function updateRuntimeSkill(
  skill: RuntimeSkillRecord,
  confirmed = true
): Promise<RuntimeSkillInstallResponse> {
  try {
    const response = await api.post<RuntimeSkillInstallResponse>('/skills/runtime', {
      skill,
      confirmed
    }, {
      timeout: RUNTIME_INSTALL_TIMEOUT_MS
    });
    return isRuntimeSkillInstallResponse(response.data)
      ? response.data
      : {
          ok: false,
          source: 'local_mock',
          message: 'invalid runtime skill update response',
          error: 'invalid runtime skill update response'
        };
  } catch (error) {
    return {
      ok: false,
      source: 'local_mock',
      message: 'runtime skill update request failed',
      error: errorMessage(error)
    };
  }
}

export async function fetchRuntimeDeviceManifests(): Promise<RuntimeDeviceManifestListResponse> {
  try {
    const response = await api.get<RuntimeDeviceManifestListResponse>('/devices/runtime', {
      timeout: 25000
    });
    return isRuntimeDeviceManifestListResponse(response.data)
      ? response.data
      : { devices: [], source: 'local_mock' };
  } catch {
    return { devices: [], source: 'local_mock' };
  }
}

export async function updateRuntimeDeviceManifest(
  device: RuntimeDeviceManifestRecord,
  confirmed = true
): Promise<RuntimeDeviceManifestUpdateResponse> {
  try {
    const response = await api.post<RuntimeDeviceManifestUpdateResponse>('/devices/runtime', {
      device,
      confirmed
    }, {
      timeout: 30000
    });
    return isRuntimeDeviceManifestUpdateResponse(response.data)
      ? response.data
      : {
          ok: false,
          source: 'local_mock',
          message: 'invalid device manifest update response',
          error: 'invalid device manifest update response'
        };
  } catch (error) {
    return {
      ok: false,
      source: 'local_mock',
      message: 'device manifest update request failed',
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
