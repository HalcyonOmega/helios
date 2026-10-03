<template>
  <Navbar></Navbar>
  <div id="content" class="container">
    <h1 class="my-4 text-center">{{ $t('type.title') }}</h1>
    <form class="form d-flex flex-column align-items-center" @submit.prevent="send(text, pressEnter)">
      <div class="card flex-column d-flex p-4 mb-4 w-100" style="max-width: 36rem">
        <p class="text-body-secondary">{{ $t('type.description') }}</p>
        <label for="type-input" class="visually-hidden">{{ $t('type.placeholder') }}</label>
        <textarea
          id="type-input"
          ref="input"
          v-model="text"
          class="form-control mb-3"
          rows="3"
          maxlength="2000"
          :placeholder="$t('type.placeholder')"
          autocomplete="off"
          autocapitalize="off"
          autocorrect="off"
          spellcheck="false"
        ></textarea>
        <div class="form-check mb-3">
          <input id="type-enter" v-model="pressEnter" class="form-check-input" type="checkbox" />
          <label for="type-enter" class="form-check-label">{{ $t('type.press_enter') }}</label>
        </div>
        <button type="submit" class="btn btn-primary mb-3" :disabled="busy || !text">
          <keyboard :size="18" class="icon"></keyboard>
          {{ $t('type.send') }}
        </button>
        <div class="d-flex gap-2 justify-content-center">
          <button type="button" class="btn btn-outline-secondary" :disabled="busy" @click="send('', true)">
            {{ $t('type.key_enter') }}
          </button>
          <button type="button" class="btn btn-outline-secondary" :disabled="busy" @click="send('\t', false)">
            {{ $t('type.key_tab') }}
          </button>
        </div>
      </div>
      <div v-if="status" :class="`alert alert-${status.type}`" role="alert">{{ status.message }}</div>
    </form>
  </div>
</template>

<script>
  import Navbar from './Navbar.vue'
  import { apiFetch } from './fetch_utils'
  import { Keyboard } from '@lucide/vue'

  export default {
    components: {
      Navbar,
      Keyboard,
    },
    inject: ['i18n'],
    data() {
      return {
        busy: false,
        pressEnter: false,
        status: null,
        text: '',
      };
    },
    mounted() {
      this.$refs.input.focus();
    },
    methods: {
      /**
       * Fetch a CSRF token: phones reach the UI by LAN address, which is not a same-origin default.
       *
       * @returns {Promise<string>} The token.
       */
      async csrfToken() {
        const response = await apiFetch('./api/csrf-token', {method: 'GET'});
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }
        return (await response.json()).csrf_token;
      },

      /**
       * Type text (and optionally Enter) into whatever has focus on the host.
       *
       * @param {string} text Text to type.
       * @param {boolean} enter Whether to press Enter afterwards.
       */
      async send(text, enter) {
        this.busy = true;
        this.status = null;
        try {
          const response = await apiFetch('./api/type', {
            method: 'POST',
            headers: {
              'Content-Type': 'application/json',
              'X-CSRF-Token': await this.csrfToken(),
            },
            body: JSON.stringify({text, enter}),
          });
          const result = await response.json();
          if (result.status === true) {
            this.status = {type: 'success', message: this.i18n.t('type.sent')};
            if (text === this.text) {
              this.text = '';
            }
          } else {
            this.status = {type: 'danger', message: result.error || this.i18n.t('type.failed')};
          }
        } catch (error) {
          console.error('Failed to type text', error);
          this.status = {type: 'danger', message: this.i18n.t('type.failed')};
        } finally {
          this.busy = false;
          this.$refs.input.focus();
        }
      },
    },
  }
</script>
