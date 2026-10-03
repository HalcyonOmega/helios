import { describe, expect, it } from 'vitest'
import { brandMessage } from '../../src_assets/common/assets/web/branding.js'

describe('Helios display branding', () => {
  it('brands greetings and controls without changing identifiers', () => {
    expect(brandMessage('Hello, Sunshine! Restart Sunshine.')).toBe('Hello, Helios! Restart Helios.')
    expect(brandMessage('sunshine.conf · SUNSHINE_SERVER_FREE')).toBe('sunshine.conf · SUNSHINE_SERVER_FREE')
  })

  it('preserves upstream documentation and license links', () => {
    expect(brandMessage('Sunshine: https://github.com/LizardByte/Sunshine'))
      .toBe('Helios: https://github.com/LizardByte/Sunshine')
  })
})
